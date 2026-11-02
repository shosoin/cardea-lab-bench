/*
 * se05x-lab-bridge — bench helper: drives the host board's BOOTSEL and RUN lines from the console,
 * so a script (or a CI job) can press the button, reset the board or put it in BOOTSEL mode
 * without anyone touching it.
 *
 * Wiring (Raspberry Pi Pico 2 as the host): BRIDGE_BENCH_BOOTSEL_GPIO --1k-- TP6 (BOOTSEL, which
 * is also the user button of pico-fido based firmware), BRIDGE_BENCH_RUN_GPIO --1k-- RUN (pin 30),
 * common GND (already there through the I2C wiring). Both pins are OPEN DRAIN: "pressed" pulls the
 * line to GND through the 1k resistor, like the board's own button; "released" leaves it floating.
 * They are never driven high: TP6 is the flash chip select and the RP2350 drives it while it runs
 * from flash. At reset and while this chip is being flashed both pins are inputs (floating).
 *
 * Console protocol (one command per line on the console UART; replies start with "BENCH"):
 *   ping                 -> BENCH OK pong
 *   press [ms]           pull BOOTSEL low for ms (default 300, max 30000), then release
 *   hold | release       pull BOOTSEL low until "release" (auto-release after 60 s)
 *   reset [ms]           pull RUN low for ms (default 100), then release: the board reboots
 *   bootsel              BOOTSEL low, RUN pulse, release BOOTSEL after 500 ms: the board comes up
 *                        in its USB bootloader
 *   status               line levels as read back on the pins (0 = low)
 * Open the console with DTR and RTS both asserted: the DevKit's auto-program circuit resets this
 * chip on other DTR/RTS combinations.
 *
 * Only for a test bench: pressing the button from software defeats user presence by design.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "driver/uart.h"
#include "esp_log.h"
#include "bench.h"

#if __has_include("config.local.h")
#include "config.local.h"
#endif
#ifndef BRIDGE_BENCH_BOOTSEL_GPIO
#define BRIDGE_BENCH_BOOTSEL_GPIO 4
#endif
#ifndef BRIDGE_BENCH_RUN_GPIO
#define BRIDGE_BENCH_RUN_GPIO 5
#endif
#define BENCH_UART            CONFIG_ESP_CONSOLE_UART_NUM
#define BENCH_PRESS_MAX_MS    30000
#define BENCH_HOLD_MAX_MS     60000

static const char *TAG = "bench";
static volatile TickType_t hold_until = 0;   /* 0 = BOOTSEL not held */

static void line_release(int gpio) {
    if (gpio >= 0) gpio_set_level(gpio, 1);   /* open drain: 1 = floating */
}

static void line_low(int gpio) {
    if (gpio >= 0) gpio_set_level(gpio, 0);
}

static void line_init(int gpio) {
    if (gpio < 0) return;
    gpio_set_level(gpio, 1);                  /* floating before the output is enabled: no glitch */
    gpio_config_t c = {
        .pin_bit_mask = 1ULL << gpio,
        .mode = GPIO_MODE_INPUT_OUTPUT_OD,    /* input too, so "status" reads the real line */
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&c);
    gpio_set_level(gpio, 1);
}

static void reply(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void reply(const char *fmt, ...) {
    char buf[128];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    printf("BENCH %s\n", buf);
    fflush(stdout);
}

static long arg_ms(const char *arg, long def, long max) {
    if (!arg || !*arg) return def;
    char *end;
    long v = strtol(arg, &end, 10);
    if (*end || v < 1 || v > max) return -1;
    return v;
}

static void run_command(char *line) {
    char *cmd = strtok(line, " \t");
    char *arg = strtok(NULL, " \t");
    if (!cmd) return;
    if (!strcmp(cmd, "ping")) {
        reply("OK pong");
    }
    else if (!strcmp(cmd, "press")) {
        long ms = arg_ms(arg, 300, BENCH_PRESS_MAX_MS);
        if (ms < 0) { reply("ERR press: ms must be 1..%d", BENCH_PRESS_MAX_MS); return; }
        hold_until = 0;
        line_low(BRIDGE_BENCH_BOOTSEL_GPIO);
        vTaskDelay(pdMS_TO_TICKS(ms));
        line_release(BRIDGE_BENCH_BOOTSEL_GPIO);
        reply("OK press %ld", ms);
    }
    else if (!strcmp(cmd, "hold")) {
        line_low(BRIDGE_BENCH_BOOTSEL_GPIO);
        hold_until = xTaskGetTickCount() + pdMS_TO_TICKS(BENCH_HOLD_MAX_MS);
        if (hold_until == 0) hold_until = 1;
        reply("OK hold");
    }
    else if (!strcmp(cmd, "release")) {
        hold_until = 0;
        line_release(BRIDGE_BENCH_BOOTSEL_GPIO);
        reply("OK release");
    }
    else if (!strcmp(cmd, "reset")) {
        long ms = arg_ms(arg, 100, 5000);
        if (ms < 0) { reply("ERR reset: ms must be 1..5000"); return; }
        line_low(BRIDGE_BENCH_RUN_GPIO);
        vTaskDelay(pdMS_TO_TICKS(ms));
        line_release(BRIDGE_BENCH_RUN_GPIO);
        reply("OK reset %ld", ms);
    }
    else if (!strcmp(cmd, "bootsel")) {
        hold_until = 0;
        line_low(BRIDGE_BENCH_BOOTSEL_GPIO);
        vTaskDelay(pdMS_TO_TICKS(20));
        line_low(BRIDGE_BENCH_RUN_GPIO);
        vTaskDelay(pdMS_TO_TICKS(100));
        line_release(BRIDGE_BENCH_RUN_GPIO);
        vTaskDelay(pdMS_TO_TICKS(500));       /* the boot ROM samples BOOTSEL right after reset */
        line_release(BRIDGE_BENCH_BOOTSEL_GPIO);
        reply("OK bootsel");
    }
    else if (!strcmp(cmd, "status")) {
        int b = BRIDGE_BENCH_BOOTSEL_GPIO >= 0 ? gpio_get_level(BRIDGE_BENCH_BOOTSEL_GPIO) : -1;
        int r = BRIDGE_BENCH_RUN_GPIO >= 0 ? gpio_get_level(BRIDGE_BENCH_RUN_GPIO) : -1;
        reply("OK status bootsel=%d run=%d held=%d", b, r, hold_until != 0);
    }
    else {
        reply("ERR unknown command '%s' (ping, press [ms], hold, release, reset [ms], bootsel, status)", cmd);
    }
}

static void bench_task(void *arg) {
    (void)arg;
    char line[64];
    size_t n = 0;
    for (;;) {
        uint8_t c;
        int got = uart_read_bytes(BENCH_UART, &c, 1, pdMS_TO_TICKS(100));
        if (hold_until && (int32_t)(xTaskGetTickCount() - hold_until) >= 0) {
            hold_until = 0;
            line_release(BRIDGE_BENCH_BOOTSEL_GPIO);
            reply("OK release (auto, after %d ms)", BENCH_HOLD_MAX_MS);
        }
        if (got != 1) continue;
        if (c == '\r' || c == '\n') {
            if (n) {
                line[n] = 0;
                run_command(line);
                n = 0;
            }
        }
        else if (n < sizeof(line) - 1) {
            line[n++] = (char)c;
        }
        else {
            n = 0;   /* overlong line: drop it */
            reply("ERR line too long");
        }
    }
}

void bench_start(void) {
    line_init(BRIDGE_BENCH_BOOTSEL_GPIO);
    line_init(BRIDGE_BENCH_RUN_GPIO);
    /* RX only; logs keep going out through the console's own path. */
    esp_err_t e = uart_driver_install(BENCH_UART, 256, 0, 0, NULL, 0);
    if (e != ESP_OK) {
        ESP_LOGE(TAG, "console UART driver: %s; bench commands disabled", esp_err_to_name(e));
        return;
    }
    xTaskCreate(bench_task, "bench", 3072, NULL, 2, NULL);
    ESP_LOGI(TAG, "bench lines: BOOTSEL GPIO%d, RUN GPIO%d (open drain); type 'ping'",
             BRIDGE_BENCH_BOOTSEL_GPIO, BRIDGE_BENCH_RUN_GPIO);
}
