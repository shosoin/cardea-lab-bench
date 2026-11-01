/*
 * se05x-lab-bridge — Raspberry Pi Pico / Pico 2 probe: talks to an SE05x (real chip or the bridge)
 * the way NXP's T=1-over-I2C stack does, and prints the ATR. Output on USB serial and UART0 (GP0).
 *
 * I2C0 on GP4 (SDA) / GP5 (SCL), 100 kHz, address 0x48.
 * 1. write the interface soft reset S-block  5a cf 00 37 7f
 * 2. poll: read 2 bytes until the first one is the SE start of frame 0xA5 (NXP retries with a delay)
 * 3. read LEN, then LEN + 2 (payload + CRC), and check the CRC-16/X-25
 *
 * Writes nothing to flash beyond its own image, so data kept by other firmware survives.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include <stdio.h>
#include "pico/stdlib.h"
#include "hardware/i2c.h"

#define SE_ADDR 0x48
#define SDA 4
#define SCL 5

static uint16_t crc_x25(const uint8_t *d, size_t n) {
    uint16_t crc = 0xFFFF;
    for (size_t i = 0; i < n; i++) {
        crc ^= d[i];
        for (int b = 0; b < 8; b++) crc = (crc & 1) ? (crc >> 1) ^ 0x8408 : crc >> 1;
    }
    return crc ^ 0xFFFF;
}

/* After an error the RP2 I2C block can stay wedged and a target may still hold SDA low. Standard
 * recovery: clock SCL nine times by hand with SDA released, send a STOP, then re-init the block. */
static void i2c_recover(void) {
    i2c_deinit(i2c0);
    gpio_init(SDA); gpio_init(SCL);
    gpio_pull_up(SDA); gpio_pull_up(SCL);
    gpio_set_dir(SDA, GPIO_IN);
    for (int i = 0; i < 9; i++) {
        gpio_set_dir(SCL, GPIO_OUT); gpio_put(SCL, 0); sleep_us(10);
        gpio_set_dir(SCL, GPIO_IN); sleep_us(10);              /* released: pulled up */
    }
    gpio_set_dir(SDA, GPIO_OUT); gpio_put(SDA, 0); sleep_us(10);   /* STOP: SDA low -> high, SCL high */
    gpio_set_dir(SDA, GPIO_IN); sleep_us(10);
    i2c_init(i2c0, 100 * 1000);
    gpio_set_function(SDA, GPIO_FUNC_I2C);
    gpio_set_function(SCL, GPIO_FUNC_I2C);
    printf("  (I2C bus recovered: SDA=%d SCL=%d)\n", gpio_get(SDA), gpio_get(SCL));
}

static int probe_once(void) {
    const uint8_t reset[] = { 0x5a, 0xcf, 0x00, 0x37, 0x7f };
    uint8_t buf[260];
    int r = i2c_write_timeout_us(i2c0, SE_ADDR, reset, sizeof(reset), false, 50000);
    if (r != (int)sizeof(reset)) { printf("write failed (%d): no device at 0x48? check wiring/pull-ups\n", r); return -1; }
    int polls = 0;
    absolute_time_t t0 = get_absolute_time();
    do {
        sleep_ms(polls < 20 ? 1 : 5);
        r = i2c_read_timeout_us(i2c0, SE_ADDR, buf, 2, false, 200000);
        polls++;
    } while ((r != 2 || buf[0] != 0xA5) && polls < 2000);
    if (buf[0] != 0xA5) { printf("no start of frame after %d polls (last read %d: %02x %02x)\n", polls, r, buf[0], buf[1]); return -1; }
    r = i2c_read_timeout_us(i2c0, SE_ADDR, buf + 2, 1, false, 200000);
    if (r != 1) { printf("LEN read failed (%d)\n", r); return -1; }
    int len = buf[2];
    r = i2c_read_timeout_us(i2c0, SE_ADDR, buf + 3, len + 2, false, 500000);
    if (r != len + 2) { printf("payload read failed (%d of %d)\n", r, len + 2); return -1; }
    uint16_t crc = crc_x25(buf, 3 + len), got = buf[3 + len] | (buf[4 + len] << 8);
    int ms = (int)(absolute_time_diff_us(t0, get_absolute_time()) / 1000);
    printf("ATR in %d ms after %d polls: NAD=%02x PCB=%02x LEN=%d CRC %s\n", ms, polls, buf[0], buf[1], len, crc == got ? "OK" : "BAD");
    printf("  vendor %02x%02x%02x%02x%02x  historical '", buf[4], buf[5], buf[6], buf[7], buf[8]);
    for (int i = 3 + len - 10; i < 3 + len; i++) putchar(buf[i] >= 32 && buf[i] < 127 ? buf[i] : '.');
    printf("'\n");
    return crc == got ? 0 : -1;
}

int main(void) {
    stdio_init_all();
    i2c_init(i2c0, 100 * 1000);
    gpio_set_function(SDA, GPIO_FUNC_I2C);
    gpio_set_function(SCL, GPIO_FUNC_I2C);
    gpio_pull_up(SDA);
    gpio_pull_up(SCL);
    sleep_ms(3000);   /* time to open the serial port */
    for (int n = 1;; n++) {
        printf("--- probe %d\n", n);
        if (probe_once() != 0) i2c_recover();
        sleep_ms(5000);
    }
}
