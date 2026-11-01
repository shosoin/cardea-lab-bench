# ESP32-S3 bridge

ESP-IDF firmware for an ESP32-S3 acting as an I2C target at address `0x48`, standing in for an NXP SE05x.

- The host MCU writes a T=1 frame; the bridge forwards it to the simulator over TCP and prepares the response.
- NXP's T=1 stack reads a response in three reads (NAD+PCB, LEN, payload+CRC) and polls again while the first byte is not `0xA5`. Until the response is ready, polls read junk from the idle target (never `0xA5`) and NXP simply retries.
- The I2C side runs in its own high-priority task; the TCP exchange runs in another one.

Measured with `tools/pico-probe` on a Pico 2 over Wi-Fi: 44/44 exchanges correct in 4 minutes, 16-21 ms from request to complete response.

## ESP32-S3 I2C target quirks (ESP-IDF 5.5, slave driver v2)

Getting byte-exact behaviour out of the S3's I2C target took three workarounds. They are why the code looks the way it does:

1. **SCL is stretched on every read address match until software releases it**, even when the FIFO already holds data. The driver raises `on_request`; the bridge answers each one at once, with the response if one is ready or with a zero-length `i2c_slave_write()`, which only releases the stretch.
2. **`i2c_slave_write()` only fills the 32-byte hardware FIFO when all the data fits.** Otherwise everything goes to a ring buffer that an interrupt drains later, at a random point relative to the host's reads (empty reads, shifted bytes). The response is written in two calls: 16 bytes that land in the FIFO before the stretch is released, then the rest.
3. **One byte is dropped after a 2-byte read** (the byte pre-fetched while the host clocks the last byte). A filler byte follows NAD+PCB (`BRIDGE_FILL_AFTER_HEADER`).

On top of that, the peripheral is **re-created for every new request**, so no byte left from a previous exchange can sit in front of the next response.
- Status goes to the USB console. The on-board RGB LED stays **off**; it only gives short, dim flashes: two green when connected to the simulator, red on errors.

## Build and flash (ESP-IDF 5.5)

```sh
cp config.local.h.example config.local.h   # Wi-Fi + simulator address; ignored by git
. $IDF_PATH/export.sh
idf.py set-target esp32s3
idf.py -p /dev/ttyACM0 flash monitor
```

Defaults: SDA = GPIO8, SCL = GPIO9, LED = GPIO48 (WS2812). Override them in `config.local.h`. The ESP32-S3 has 2.4 GHz Wi-Fi only.
