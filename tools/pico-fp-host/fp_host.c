/*
 * cardea-lab-bench — Raspberry Pi Pico 2 test host for a GROW R502 family fingerprint module (the
 * real one or the bench's fp_emu): it exposes the module's lines on USB serial, so a script on the
 * PC drives the module the way the key's firmware will. Same pins as the Cardea Key PCB:
 *
 *   GP8  UART1 TX  -> module RXD          GP10 WAKEUP <- module (low = finger), no pulls (floats high)
 *   GP9  UART1 RX  <- module TXD          GP11 main power switch (0 = module powered, as the PCB's
 *                                              P-MOSFET gate); starts at 1 (off)
 *
 * USB serial, one command per line; answers "OK ..." / "ERR ...":
 *   ping | baud N | pwr 0|1 (1 = powered) | tx <hex> | wake | rxpin | wakepin
 *     rxpin: level of GP9 with a pull-down (0 = the module's TX floats, 1 = it drives idle)
 * Asynchronous lines: "RX <hex>" with bytes from the module, "WAKE <0|1>" when WAKEUP changes.
 *
 * Writes nothing to flash beyond its own image, so data kept by other firmware survives.
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "pico/stdlib.h"
#include "hardware/uart.h"
#include "hardware/irq.h"
#include "hardware/gpio.h"

#define FP_UART  uart1
#define PIN_TX   8
#define PIN_RX   9
#define PIN_WAKE 10
#define PIN_PWR  11

#define RB_SIZE 8192
static volatile uint8_t rb[RB_SIZE];
static volatile uint32_t rb_head, rb_tail;
static volatile uint32_t rb_lost;

static void on_uart_rx(void) {
    while (uart_is_readable(FP_UART)) {
        uint8_t c = (uint8_t)uart_get_hw(FP_UART)->dr;
        uint32_t next = (rb_head + 1) % RB_SIZE;
        if (next == rb_tail) { rb_lost++; continue; }
        rb[rb_head] = c;
        rb_head = next;
    }
}

static int hexval(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static void cmd(char *line) {
    char *arg = strchr(line, ' ');
    if (arg) *arg++ = 0;
    if (!strcmp(line, "ping")) {
        printf("OK pong fp_host lost=%lu\n", (unsigned long)rb_lost);
    }
    else if (!strcmp(line, "baud") && arg) {
        unsigned b = uart_set_baudrate(FP_UART, (unsigned)atoi(arg));
        printf("OK baud %u\n", b);
    }
    else if (!strcmp(line, "pwr") && arg) {
        gpio_put(PIN_PWR, arg[0] == '1' ? 0 : 1);
        printf("OK pwr %c\n", arg[0] == '1' ? '1' : '0');
    }
    else if (!strcmp(line, "tx") && arg) {
        size_t n = 0;
        static uint8_t buf[512];
        for (char *p = arg; p[0] && p[1] && n < sizeof(buf); ) {
            if (*p == ' ') { p++; continue; }
            int h = hexval(p[0]), l = hexval(p[1]);
            if (h < 0 || l < 0) { printf("ERR tx: bad hex\n"); return; }
            buf[n++] = (uint8_t)(h << 4 | l);
            p += 2;
        }
        uart_write_blocking(FP_UART, buf, n);
        printf("OK tx %u\n", (unsigned)n);
    }
    else if (!strcmp(line, "wake")) {
        printf("OK wake %d\n", gpio_get(PIN_WAKE));
    }
    else if (!strcmp(line, "rxpin")) {
        /* RP2350-E9: with the input buffer on, a pad that was high stays at ~2 V against the
         * internal pull-down. Pull down with the input buffer off, then read right away. */
        gpio_set_function(PIN_RX, GPIO_FUNC_SIO);
        gpio_set_dir(PIN_RX, GPIO_IN);
        gpio_set_input_enabled(PIN_RX, false);
        gpio_pull_down(PIN_RX);
        sleep_ms(5);
        gpio_set_input_enabled(PIN_RX, true);
        busy_wait_us(5);
        int v = gpio_get(PIN_RX);
        gpio_disable_pulls(PIN_RX);
        gpio_pull_up(PIN_RX);
        sleep_ms(1);                           /* line back high before the UART sees it */
        gpio_set_function(PIN_RX, GPIO_FUNC_UART);
        while (uart_is_readable(FP_UART)) (void)uart_getc(FP_UART);
        printf("OK rxpin %d\n", v);
    }
    else if (!strcmp(line, "wakepin")) {
        /* WAKEUP under each test (cardea-key-dev#52: telling a removed sensor from a touch):
         * plain = no pulls; pd / pu = brief pull with the input buffer off while it settles, read
         * right away (RP2350-E9); pd_on = pull-down with the buffer on for 2 ms (what E9 spoils). */
        int plain = gpio_get(PIN_WAKE);
        int r[2];
        for (int k = 0; k < 2; k++) {
            gpio_set_input_enabled(PIN_WAKE, false);
            if (k == 0) gpio_pull_down(PIN_WAKE); else gpio_pull_up(PIN_WAKE);
            sleep_ms(5);
            gpio_set_input_enabled(PIN_WAKE, true);
            busy_wait_us(5);
            r[k] = gpio_get(PIN_WAKE);
            gpio_disable_pulls(PIN_WAKE);
            sleep_ms(1);
        }
        gpio_pull_down(PIN_WAKE);
        sleep_ms(2);
        int pd_on = gpio_get(PIN_WAKE);
        gpio_disable_pulls(PIN_WAKE);
        sleep_ms(1);
        printf("OK wakepin plain=%d pd=%d pu=%d pd_on=%d\n", plain, r[0], r[1], pd_on);
    }
    else {
        printf("ERR unknown (ping, baud N, pwr 0|1, tx HEX, wake, rxpin, wakepin)\n");
    }
}

int main(void) {
    gpio_init(PIN_PWR);
    gpio_put(PIN_PWR, 1);                      /* module off until asked */
    gpio_set_dir(PIN_PWR, GPIO_OUT);
    gpio_init(PIN_WAKE);
    gpio_set_dir(PIN_WAKE, GPIO_IN);
    gpio_disable_pulls(PIN_WAKE);              /* no pulls: what works with the real module (RP2350-E9) */

    stdio_init_all();
    uart_init(FP_UART, 57600);
    uart_set_format(FP_UART, 8, 1, UART_PARITY_NONE);
    uart_set_fifo_enabled(FP_UART, true);
    gpio_set_function(PIN_TX, GPIO_FUNC_UART);
    gpio_set_function(PIN_RX, GPIO_FUNC_UART);
    gpio_pull_up(PIN_RX);
    irq_set_exclusive_handler(UART1_IRQ, on_uart_rx);
    irq_set_enabled(UART1_IRQ, true);
    uart_set_irq_enables(FP_UART, true, false);

    char line[1100];
    size_t n = 0;
    int wake = gpio_get(PIN_WAKE);
    absolute_time_t last_rx = get_absolute_time();
    for (;;) {
        int c = getchar_timeout_us(0);
        if (c >= 0) {
            if (c == '\r' || c == '\n') {
                if (n) { line[n] = 0; cmd(line); n = 0; }
            }
            else if (n < sizeof(line) - 1) line[n++] = (char)c;
        }
        /* Bytes from the module: one line per burst (flushed after 3 ms of silence or 64 bytes) */
        uint32_t avail = (rb_head - rb_tail + RB_SIZE) % RB_SIZE;
        if (avail && (avail >= 64 || absolute_time_diff_us(last_rx, get_absolute_time()) > 3000)) {
            printf("RX ");
            for (uint32_t i = 0; i < avail && i < 64; i++) {
                printf("%02X", rb[rb_tail]);
                rb_tail = (rb_tail + 1) % RB_SIZE;
            }
            printf("\n");
        }
        static uint32_t prev_head;
        if (rb_head != prev_head) { prev_head = rb_head; last_rx = get_absolute_time(); }
        int w = gpio_get(PIN_WAKE);
        if (w != wake) { wake = w; printf("WAKE %d\n", w); }
    }
}
