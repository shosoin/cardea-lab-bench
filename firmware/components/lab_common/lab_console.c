/*
 * Lab bench — command console. One command per line on the console UART (the DevKit's USB-UART);
 * every reply is one line starting with "BENCH", so a host script can skip the log lines that share
 * the same port. Each module registers its own commands; this file only reads lines and dispatches.
 *
 * Open the console with DTR and RTS both asserted: the DevKit's auto-program circuit resets the
 * ESP32-S3 on other DTR/RTS combinations.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "driver/uart.h"
#include "esp_log.h"
#include "sdkconfig.h"
#include "lab_console.h"

#define CONSOLE_UART CONFIG_ESP_CONSOLE_UART_NUM

static const char *TAG = "console";

typedef struct {
    const char *name;
    const char *usage;
    lab_cmd_fn fn;
} lab_cmd_t;

static lab_cmd_t cmds[LAB_CONSOLE_MAX_CMDS];
static int n_cmds;
static SemaphoreHandle_t reply_lock;

void lab_console_register(const char *name, const char *usage, lab_cmd_fn fn) {
    if (n_cmds >= LAB_CONSOLE_MAX_CMDS) {
        ESP_LOGE(TAG, "too many commands, '%s' not registered", name);
        return;
    }
    cmds[n_cmds++] = (lab_cmd_t){ name, usage, fn };
}

void lab_console_reply(const char *fmt, ...) {
    char buf[160];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (reply_lock) xSemaphoreTake(reply_lock, portMAX_DELAY);
    printf("BENCH %s\n", buf);
    fflush(stdout);
    if (reply_lock) xSemaphoreGive(reply_lock);
}

static void cmd_ping(char *arg) {
    (void)arg;
    lab_console_reply("OK pong");
}

static void unknown(const char *cmd) {
    char list[160];
    size_t k = 0;
    list[0] = 0;
    for (int i = 0; i < n_cmds && k < sizeof(list); i++) {
        k += snprintf(list + k, sizeof(list) - k, "%s%s", i ? ", " : "", cmds[i].usage);
    }
    lab_console_reply("ERR unknown command '%s' (%s)", cmd, list);
}

static void dispatch(char *line) {
    char *cmd = strtok(line, " \t");
    if (!cmd) return;
    char *arg = strtok(NULL, "");
    if (arg) {
        while (*arg == ' ' || *arg == '\t') arg++;
        if (!*arg) arg = NULL;
    }
    for (int i = 0; i < n_cmds; i++) {
        if (!strcmp(cmd, cmds[i].name)) {
            cmds[i].fn(arg);
            return;
        }
    }
    unknown(cmd);
}

static void console_task(void *arg) {
    (void)arg;
    char line[96];
    size_t n = 0;
    for (;;) {
        uint8_t c;
        if (uart_read_bytes(CONSOLE_UART, &c, 1, portMAX_DELAY) != 1) continue;
        if (c == '\r' || c == '\n') {
            if (n) {
                line[n] = 0;
                dispatch(line);
                n = 0;
            }
        }
        else if (n < sizeof(line) - 1) {
            line[n++] = (char)c;
        }
        else {
            n = 0;   /* overlong line: drop it */
            lab_console_reply("ERR line too long");
        }
    }
}

void lab_console_start(void) {
    reply_lock = xSemaphoreCreateMutex();
    lab_console_register("ping", "ping", cmd_ping);
    /* RX only; logs keep going out through the console's own path. */
    esp_err_t e = uart_driver_install(CONSOLE_UART, 256, 0, 0, NULL, 0);
    if (e != ESP_OK) {
        ESP_LOGE(TAG, "console UART driver: %s; commands disabled", esp_err_to_name(e));
        return;
    }
    xTaskCreate(console_task, "console", 4096, NULL, 2, NULL);
    ESP_LOGI(TAG, "%d commands; type 'ping'", n_cmds);
}
