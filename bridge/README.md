# ESP32-S3 bridge (in progress)

Firmware for an ESP32-S3 acting as an I2C target at address `0x48`. It collects the T=1 frame written by the host MCU, forwards it to the simulator over TCP, NACKs reads until the response arrives, then serves it. Status and errors go to the USB console and the on-board RGB LED.
