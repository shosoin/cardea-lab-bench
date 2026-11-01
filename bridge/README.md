# ESP32-S3 bridge

ESP-IDF firmware for an ESP32-S3 acting as an I2C target at address `0x48`, standing in for an NXP SE05x.

- The host MCU writes a T=1 frame; the bridge forwards it to the simulator over TCP and keeps the response.
- NXP's T=1 stack reads a response in three reads (NAD+PCB, LEN, payload+CRC) and polls again while the first byte is not `0xA5`. While the simulator has not answered, every poll gets `00 00`; then the whole frame is queued in the I2C FIFO.
- The I2C side runs in its own high-priority task, so polls are answered immediately while the TCP exchange runs in another task.
- Status goes to the USB console. The on-board RGB LED stays **off**; it only gives short, dim flashes: two green when connected to the simulator, red on errors.

## Build and flash (ESP-IDF 5.5)

```sh
cp config.local.h.example config.local.h   # Wi-Fi + simulator address; ignored by git
. $IDF_PATH/export.sh
idf.py set-target esp32s3
idf.py -p /dev/ttyACM0 flash monitor
```

Defaults: SDA = GPIO8, SCL = GPIO9, LED = GPIO48 (WS2812). Override them in `config.local.h`. The ESP32-S3 has 2.4 GHz Wi-Fi only.
