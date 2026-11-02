/*
 * Lab bench — status LED (the DevKit's WS2812). It stays OFF and only gives short, dim flashes, so
 * a board on a desk does not light up the room.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */
#include <stdint.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "led_strip.h"
#include "sdkconfig.h"
#include "lab_led.h"

static led_strip_handle_t led;

void lab_led_init(void) {
    if (CONFIG_LAB_LED_GPIO < 0) return;
    led_strip_config_t cfg = { .strip_gpio_num = CONFIG_LAB_LED_GPIO, .max_leds = 1 };
    led_strip_rmt_config_t rmt = { .resolution_hz = 10 * 1000 * 1000 };
    if (led_strip_new_rmt_device(&cfg, &rmt, &led) == ESP_OK) {
        led_strip_clear(led);   /* some boards light the LED at power-up: switch it off */
    }
    else {
        led = NULL;
    }
}

/* One short, dim flash (max 12/255 per channel), then off. */
void lab_led_flash(lab_led_event_t ev, int times) {
    if (!led) return;
    uint8_t r = 0, g = 0, b = 0;
    if (ev == LAB_LED_OK) g = 12; else if (ev == LAB_LED_WARN) { r = 10; g = 6; } else r = 12;
    for (int i = 0; i < times; i++) {
        led_strip_set_pixel(led, 0, r, g, b);
        led_strip_refresh(led);
        vTaskDelay(pdMS_TO_TICKS(60));
        led_strip_clear(led);
        vTaskDelay(pdMS_TO_TICKS(140));
    }
}
