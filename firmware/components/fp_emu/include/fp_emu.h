/* Lab bench — fingerprint sensor module emulator (GROW R502-F Pro). See fp_emu.c. */
#ifndef FP_EMU_H
#define FP_EMU_H

/* Loads the emulated module's flash (NVS: call after nvs_flash_init), sets up its UART and lines
 * and registers the "fp" console command. */
void fp_emu_start(void);

#endif
