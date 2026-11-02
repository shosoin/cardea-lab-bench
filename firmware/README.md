# Lab bench firmware (ESP32-S3)

ESP-IDF 5.5 firmware that turns one ESP32-S3 board into test equipment for a host board (here, a Raspberry Pi Pico 2 running Cardea Key). **Lab prototype for testing and test automation, not a security device.**

```
firmware/
  main/main.c                 starts the enabled modules
  components/
    lab_common/               status LED + command console (one command per line, replies "BENCH ...")
    se05x_bridge/             I2C target standing in for an NXP SE05x, relaying to SE050Sim
    bench_io/                 BOOTSEL / user button and RUN lines of the host
  config.local.h              Wi-Fi + simulator address (from config.local.h.example; ignored by git)
```

Each module has its own `Kconfig` menu ("Lab bench: ...") with an on/off switch and its pins; `sdkconfig.defaults` turns them all on. A module that is off is not compiled and registers no console commands. A planned third module, `fp_emu`, will emulate the fingerprint sensor ([#2](https://github.com/shosoin/se05x-lab-bridge/issues/2)).

| Module | Default pins |
|---|---|
| `se05x_bridge` | SDA GPIO8, SCL GPIO9, address `0x48` |
| `bench_io` | BOOTSEL GPIO4, RUN GPIO5 |
| `lab_common` | WS2812 LED GPIO48 (`-1`: none); console on the DevKit's USB-UART |

## Module `se05x_bridge`

An I2C target at address `0x48`, standing in for an NXP SE05x.

- The host MCU writes a T=1 frame; the bridge forwards it to the simulator over TCP and prepares the response.
- NXP's T=1 stack reads a response in three reads (NAD+PCB, LEN, payload+CRC) and polls again while the first byte is not `0xA5`. Until the response is ready, polls read junk from the idle target (never `0xA5`) and NXP simply retries.
- The I2C side runs in its own high-priority task; the TCP exchange runs in another one.

Measured with `tools/pico-probe` on a Pico 2 over Wi-Fi: 44/44 exchanges correct in 4 minutes, 16-21 ms from request to complete response.

### ESP32-S3 I2C target quirks (ESP-IDF 5.5, slave driver v2)

Getting byte-exact behaviour out of the S3's I2C target took three workarounds. They are why the code looks the way it does:

1. **SCL is stretched on every read address match until software releases it**, even when the FIFO already holds data. The driver raises `on_request`; the bridge answers each one at once, with the response if one is ready or with a zero-length `i2c_slave_write()`, which only releases the stretch.
2. **`i2c_slave_write()` only fills the 32-byte hardware FIFO when all the data fits.** Otherwise everything goes to a ring buffer that an interrupt drains later, at a random point relative to the host's reads (empty reads, shifted bytes). The response is written in two calls: 16 bytes that land in the FIFO before the stretch is released, then the rest.
3. **One byte is dropped after a 2-byte read** (the byte pre-fetched while the host clocks the last byte). A filler byte follows NAD+PCB (`CONFIG_LAB_SE05X_FILL_AFTER_HEADER`).

On top of that, the peripheral is **re-created for every new request**, so no byte left from a previous exchange can sit in front of the next response, and a **watchdog** re-creates it if SCL stays low for 1.5 s (a host reset or re-flashed in the middle of a read would otherwise leave the bus stuck, and the host would just see no secure element).
- Status goes to the USB console. The on-board RGB LED stays **off**; it only gives short, dim flashes: two green when connected to the simulator, red on errors.

## Build and flash (ESP-IDF 5.5)

```sh
cp config.local.h.example config.local.h   # Wi-Fi + simulator address; ignored by git
. $IDF_PATH/export.sh
idf.py set-target esp32s3
idf.py menuconfig                          # optional: "Lab bench: ..." menus (modules, pins)
idf.py -p /dev/ttyACM0 flash monitor
```

The ESP32-S3 has 2.4 GHz Wi-Fi only.

## Module `bench_io`: BOOTSEL and RUN of the host board

A "finger" for unattended tests (`components/bench_io/`).
Two GPIOs are open-drain outputs, each through a **1 kΩ** series resistor:

| ESP32-S3 | Host (Raspberry Pi Pico 2) |
|---|---|
| GPIO4 (`CONFIG_LAB_BENCH_BOOTSEL_GPIO`) | TP6, the BOOTSEL line (also the user button of pico-fido based firmware) |
| GPIO5 (`CONFIG_LAB_BENCH_RUN_GPIO`) | RUN, pin 30 |
| GND | GND (already shared with the I2C wiring) |

"Pressed" pulls the line to GND, like the board's button; "released" leaves it floating. The pins
are never driven high (TP6 is the flash chip select), and they float while the S3 boots or is
flashed. Console commands: `press [ms]`, `hold`, `release` (a hold auto-releases after 60 s),
`reset [ms]`, `bootsel`, `status`.

Open the console with **DTR and RTS both asserted**. On the DevKit, other combinations go through
the auto-program circuit and reset the S3 (and drop the simulator connection).

This is for a test bench only: pressing the button from software defeats user presence.

## Console (`lab_common`)

One command per line on the console UART; every reply is one line starting with `BENCH` (`BENCH OK ...` or `BENCH ERR ...`), so a host script can skip the log lines that share the port. Built in: `ping`. An unknown command answers with the list of the registered ones. A module adds its commands with `lab_console_register()`.
