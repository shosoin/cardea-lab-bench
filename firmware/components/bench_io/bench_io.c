/*
 * Lab bench — drives the host board's BOOTSEL and RUN lines from the console, so a script (or a CI
 * job) can press the button, reset the board or put it in BOOTSEL mode without anyone touching it.
 *
 * Wiring (Raspberry Pi Pico 2 as the host): LAB_BENCH_BOOTSEL_GPIO --1k-- TP6 (BOOTSEL, which is
 * also the user button of pico-fido based firmware), LAB_BENCH_RUN_GPIO --1k-- RUN (pin 30), common
 * GND. Both pins are OPEN DRAIN: "pressed" pulls the line to GND through the 1k resistor, like the
 * board's own button; "released" leaves it floating. They are never driven high: TP6 is the flash
 * chip select and the RP2350 drives it while it runs from flash. At reset and while this chip is
 * being flashed both pins are inputs (floating).
 *
 * Console commands (replies start with "BENCH"):
 *   press [ms]           pull BOOTSEL low for ms (default 300, max 30000), then release
 *   hold | release       pull BOOTSEL low until "release" (auto-release after 60 s)
 *   reset [ms]           pull RUN low for ms (default 100), then release: the board reboots
 *   bootsel              BOOTSEL low, RUN pulse, release BOOTSEL after 500 ms: the board comes up
 *                        in its USB bootloader
 *   status               line levels as read back on the pins (0 = low)
 *
 * Only for a test bench: pressing the button from software defeats user presence by design.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */
#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/timers.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "sdkconfig.h"
#include "lab_console.h"
#include "bench_io.h"

#define BOOTSEL_GPIO       CONFIG_LAB_BENCH_BOOTSEL_GPIO
#define RUN_GPIO           CONFIG_LAB_BENCH_RUN_GPIO
#define PRESS_MAX_MS       30000
#define HOLD_MAX_MS        60000

static const char *TAG = "bench_io";
static TimerHandle_t hold_timer;   /* running = BOOTSEL held */

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

static long arg_ms(const char *arg, long def, long max) {
    if (!arg || !*arg) return def;
    char *end;
    long v = strtol(arg, &end, 10);
    if (*end || v < 1 || v > max) return -1;
    return v;
}

static void stop_hold(void) {
    xTimerStop(hold_timer, portMAX_DELAY);
}

static void hold_expired(TimerHandle_t t) {
    (void)t;
    line_release(BOOTSEL_GPIO);
    lab_console_reply("OK release (auto, after %d ms)", HOLD_MAX_MS);
}

static void cmd_press(char *arg) {
    long ms = arg_ms(arg, 300, PRESS_MAX_MS);
    if (ms < 0) { lab_console_reply("ERR press: ms must be 1..%d", PRESS_MAX_MS); return; }
    stop_hold();
    line_low(BOOTSEL_GPIO);
    vTaskDelay(pdMS_TO_TICKS(ms));
    line_release(BOOTSEL_GPIO);
    lab_console_reply("OK press %ld", ms);
}

static void cmd_hold(char *arg) {
    (void)arg;
    line_low(BOOTSEL_GPIO);
    xTimerReset(hold_timer, portMAX_DELAY);
    lab_console_reply("OK hold");
}

static void cmd_release(char *arg) {
    (void)arg;
    stop_hold();
    line_release(BOOTSEL_GPIO);
    lab_console_reply("OK release");
}

static void cmd_reset(char *arg) {
    long ms = arg_ms(arg, 100, 5000);
    if (ms < 0) { lab_console_reply("ERR reset: ms must be 1..5000"); return; }
    line_low(RUN_GPIO);
    vTaskDelay(pdMS_TO_TICKS(ms));
    line_release(RUN_GPIO);
    lab_console_reply("OK reset %ld", ms);
}

static void cmd_bootsel(char *arg) {
    (void)arg;
    stop_hold();
    line_low(BOOTSEL_GPIO);
    vTaskDelay(pdMS_TO_TICKS(20));
    line_low(RUN_GPIO);
    vTaskDelay(pdMS_TO_TICKS(100));
    line_release(RUN_GPIO);
    vTaskDelay(pdMS_TO_TICKS(500));           /* the boot ROM samples BOOTSEL right after reset */
    line_release(BOOTSEL_GPIO);
    lab_console_reply("OK bootsel");
}

static void cmd_status(char *arg) {
    (void)arg;
    int b = BOOTSEL_GPIO >= 0 ? gpio_get_level(BOOTSEL_GPIO) : -1;
    int r = RUN_GPIO >= 0 ? gpio_get_level(RUN_GPIO) : -1;
    lab_console_reply("OK status bootsel=%d run=%d held=%d", b, r, xTimerIsTimerActive(hold_timer) != pdFALSE);
}

void bench_io_start(void) {
    line_init(BOOTSEL_GPIO);
    line_init(RUN_GPIO);
    hold_timer = xTimerCreate("hold", pdMS_TO_TICKS(HOLD_MAX_MS), pdFALSE, NULL, hold_expired);
    lab_console_register("press", "press [ms]", cmd_press);
    lab_console_register("hold", "hold", cmd_hold);
    lab_console_register("release", "release", cmd_release);
    lab_console_register("reset", "reset [ms]", cmd_reset);
    lab_console_register("bootsel", "bootsel", cmd_bootsel);
    lab_console_register("status", "status", cmd_status);
    ESP_LOGI(TAG, "BOOTSEL GPIO%d, RUN GPIO%d (open drain)", BOOTSEL_GPIO, RUN_GPIO);
}
