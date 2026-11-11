/*
 * Lab bench firmware for an ESP32-S3: test equipment for firmware running on a host board (here,
 * a Raspberry Pi Pico 2 running Cardea Key), so its tests can run unattended.
 *
 * Modules, each one switchable in menuconfig ("Lab bench: ..."):
 *   se05x_bridge  stands in for an NXP SE05x secure element on I2C (relays to SE050Sim)
 *   bench_io      presses the host's button / BOOTSEL and resets it (RUN)
 *   fp_emu        stands in for the GROW R502-F Pro fingerprint module on a UART
 * They share the status LED and the command console (lab_common).
 *
 * This is a lab prototype, not a security device: it exists to defeat, on purpose, what a real
 * device must enforce (user presence, a real secure element, a real finger).
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */
#include "nvs_flash.h"
#include "sdkconfig.h"
#include "lab_led.h"
#include "lab_console.h"
#if CONFIG_LAB_BENCH_IO
#include "bench_io.h"
#endif
#if CONFIG_LAB_SE05X_BRIDGE
#include "se05x_bridge.h"
#endif
#if CONFIG_LAB_FP_EMU
#include "fp_emu.h"
#endif

void app_main(void) {
    lab_led_init();
#if CONFIG_LAB_BENCH_IO
    bench_io_start();                            /* BOOTSEL/RUN lines released as early as possible */
#endif
    lab_console_start();
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        nvs_flash_init();
    }
#if CONFIG_LAB_FP_EMU
    fp_emu_start();                              /* after NVS: its "flash" lives there */
#endif
#if CONFIG_LAB_SE05X_BRIDGE
    se05x_bridge_start();
#endif
}
