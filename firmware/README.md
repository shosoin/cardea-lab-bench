# Lab bench firmware (ESP32-S3)

ESP-IDF 5.5 firmware that turns one ESP32-S3 board into test equipment for a host board (here, a Raspberry Pi Pico 2 running Cardea Key). **Lab prototype for testing and test automation, not a security device.**

```
firmware/
  main/main.c                 starts the enabled modules
  components/
    lab_common/               status LED + command console (one command per line, replies "BENCH ...")
    se05x_bridge/             I2C target standing in for an NXP SE05x, relaying to SE050Sim
    bench_io/                 BOOTSEL / user button and RUN lines of the host
    fp_emu/                   GROW R502-F Pro fingerprint module emulator (UART, WAKEUP, power)
  config.local.h              Wi-Fi + simulator address (from config.local.h.example; ignored by git)
```

Each module has its own `Kconfig` menu ("Lab bench: ...") with an on/off switch and its pins; `sdkconfig.defaults` turns them all on. A module that is off is not compiled and registers no console commands.

| Module | Default pins |
|---|---|
| `se05x_bridge` | SDA GPIO8, SCL GPIO9, address `0x48` |
| `bench_io` | BOOTSEL GPIO4, RUN GPIO5 |
| `fp_emu` | UART1 TX GPIO6, RX GPIO7, WAKEUP GPIO15, power (input) GPIO16 |
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

## Module `fp_emu`: GROW R502-F Pro fingerprint module

Stands in for the fingerprint module (`components/fp_emu/`), so the host's fingerprint code can be
written and tested without the sensor, and without a person touching it. Written from the module's
user manual, *R502F-Pro 指纹识别模块用户手册* V1.1 (March 2024, from hzgrow.com's download page);
the real module is the reference and wins where they differ.

| ESP32-S3 | Host (Raspberry Pi Pico 2, same GPIOs as the Cardea Key PCB) | Module pin it replaces |
|---|---|---|
| GPIO6, UART1 TX | GP9, UART1 RX (220 Ω in series) | 3, TXD |
| GPIO7, UART1 RX | GP8, UART1 TX (1 kΩ in series) | 4, RXD |
| GPIO15 | GP10 (220 Ω; 4.7 kΩ to GND on the host side, RP2350-E9) | 5, WAKEUP |
| GPIO16 (input) | GP11, the host's power switch (on the PCB, the P-MOSFET gate: 0 = on) | 1, VCC (switched) |
| GND | GND | 2, GND |

**What it does**, all from the manual:

- UART at 57600 8N1 (9600 × N with N = 1/2/4/6/12 after `SetSysPara`, from the next power-on);
  packets `EF01` | address | packet id | length | payload | checksum.
- The 31 documented commands: `GetImage`, `GenChar`, `Match`, `Search`, `RegModel`, `StoreChar`,
  `LoadChar`, `UpChar`, `DownChar`, `UpImage`, `DownImage`, `DeleteChar`, `Empty`, `SetSysPara`,
  `ReadSysPara`, `SetPwd`, `VfyPwd`, `GetRandomCode`, `SetAddr`, `ReadInfPage`, `WriteNotepad`,
  `ReadNotepad`, `ValidTemplateNum`, `ReadIndexTable`, `Cancel`, `HandShake`, `GetAlgVer`,
  `GetFwVer`, `AuraLedConfig`, `AutoEnroll` (6 captures, steps 0x01–0x0F, overwrite / duplicate /
  step report / lift options) and `AutoIdentify` (steps 1–3, retry count). Others answer `0xFC`.
- RAM lost at power-off (image buffer 192 × 192 at 4 bits per pixel, six character buffers) and
  "flash" kept in NVS (1500-slot library, password, address, parameters, 512-byte notepad).
- **Password**: once set, the first command after power-on must be `VfyPwd` (others answer
  `0x21`). There is **no link encryption**: the R502-F Pro has none (other GROW models, e.g. the
  R558-S, do have key commands).
- **Power**: with GPIO16 "off" it answers nothing and its TX floats; at power-on it waits 50 ms
  and sends `0x55`. Switching it back on less than 2 s after switching it off is logged and
  counted (the manual forbids it).
- **WAKEUP** is low while a finger is on the sensor, also with the main supply off (the touch
  circuit has its own supply). "Unplugged" leaves it floating.
- The LED ring commands the host sends, and the module's own signalling during `AutoEnroll` /
  `AutoIdentify` (charts 6.3 / 6.4), are logged for the tests to check.

**Fingers are synthetic.** A finger is a number (`A`..`Z` = 1..26); its images and templates are
generated from it, so a finger matches itself and nothing else. Templates are 512 bytes (`FPE1`,
kind, count, finger, filler, sum) and images carry a small header (`FPI1`, finger, quality), so
data uploaded from the emulator can be downloaded again and still match. Data in any other format
is accepted but never matches (`StoreChar` of it answers `0x1B`).

**Console** (replies `BENCH OK fp ...`):

| Command | |
|---|---|
| `fp status` | power, finger, baud rate, address, password, library size, operation in progress |
| `fp finger <F> [good\|messy\|small\|fail]` | put finger F on the sensor. `messy` / `small`: image taken, no features (`0x06` / `0x07`); `fail`: no image (`0x03`) |
| `fp finger off`, `fp lift` | lift it |
| `fp tap <F> [n] [on_ms] [off_ms] [quality]` | n touches in the background (default 1, 600, 600), e.g. for `AutoEnroll` with "lift between captures" |
| `fp power [on\|off\|pin]` | force the main supply, or follow GPIO16 (default) |
| `fp unplug`, `fp plug` | module disconnected: no answers, TX and WAKEUP float |
| `fp next <cmd\|any> <fault> [arg]` | fault on the next answer to that command (hex code or name): `code XX`, `silent`, `badsum`, `short` (half a packet), `garbage` (noise before it), `delay MS`, `reboot` (the module restarts instead of answering), `page N` (answers "ok, slot N" whatever range was asked: a `Search` that ignored its range). Up to 8 queued; `fp next clear` |
| `fp led [clear]` | last LED ring events, newest first: `host:breath/80/7col/0`, `module:on/0/green/0` |
| `fp db`, `fp db set <slot> <F>`, `fp db del <slot>` | library: list / add / delete without going through the protocol |
| `fp factory` | erase the emulated flash (library, password, address, parameters, notepad) |
| `fp opt speed fast\|real`, `score N`, `timeout MS`, `trace 0\|1\|2`, `pwdgate 0\|1` | processing delays off/on (default real), `MatchScore` of a capture against its finger (150), timeout of the automatic operations (10 s), log level (2 = every byte), require `VfyPwd` first when a password is set (default 0, as the real module) |
| `fp stats [clear]` | commands, bad checksums, other addresses, bad lengths, wrong passwords, power-ons, too-fast power cycles, faults |

**Measured on the real R502-F Pro** (2026-11-13, cardea-key-dev#42; the emulator does the same):

| Case | Real module |
|---|---|
| Password set, after a power cut | gates nothing: every command answers without `VfyPwd`; `VfyPwd` only says `0x00` / `0x13`, no lockout (55 wrong ones tried). `fp opt pwdgate 1` emulates a module that would gate |
| `SetSysPara` baud rate (parameter 4) | applies at once, after the answer |
| `DownChar` of data that is not a template | answers `0x00`, drops it in silence; the buffer keeps what it held (so `StoreChar` stores that) |
| `DownChar` of a template with a byte changed | taken as sent; `StoreChar` re-encodes it (`UpChar` of the slot gives other bytes) |
| `Match` | a template against itself: 409, always; two templates of the same finger from different enrolments: no match (0, now and then 35-52); a fresh capture against its finger: match |
| `Empty` | does not clear the notepad |
| `ReadSysPara` system identifier | 600 |
| WAKEUP | idle high through ~10 kΩ, low with a finger, also with VCC off (only 3.3VT); a cut line floats (reads 0 only under a brief pull-down with the input buffer off, RP2350-E9) |
| Versions | `GetFwVer` `V1.0.1`, `GetAlgVer` `V3.5.3` (the emulator keeps its own strings) |

**Choices where the manual is silent or contradicts itself**, marked `UNVERIFIED` in the code, to
check against the real module (the tests in `tools/fp-test` run unchanged against it through
`tools/pico-fp-host`):

| Case | Emulator | Why |
|---|---|---|
| Packet for another address | no answer | the manual says the module "rejects" it; code `0x20` exists but answering on a shared line would collide |
| Command while `AutoEnroll` / `AutoIdentify` runs (except `Cancel`) | ignored, no answer | not described |
| `Cancel` | `0x00`; the cancelled operation sends nothing more | not described |
| Timeout of the automatic operations | 10 s (`fp opt timeout`) | text says 10 s, charts say 15 s |
| `AutoEnroll` with the library full | `0x1F` | table says `0x1F`, chart says `0x0B` |
| `AutoEnroll` on a used slot without overwrite | `0x22` | chart says `0x22`; the table calls `0x22` "template empty" |
| `AutoEnroll` step packets | carry the slot number in every step | the manual's example does; its table shows `0x0000` until the last step |
| `AutoEnroll` waiting for the finger to lift | also times out | the manual only times out a missing finger |
| `AutoIdentify`: features fail | `0x07`, counts as a failed attempt | `0x07` is not in its list |
| `AutoIdentify` retry | waits for the finger to lift and touch again | otherwise one long touch would use up all retries at once |
| `UpChar` / `DownChar` buffer id | uses the id given (1..6) | the manual says the id is unused, then uses buffer 2 in an example |
| `RegModel` | merges every buffer holding a single-image feature (needs ≥ 2), result in buffer 1, clears 2..6 | it does not say which buffers |
| `GenChar` / `Search` / `StoreChar` / `LoadChar` with buffer id outside 1..6 | `0x01` | not listed |
| `SetSysPara` with a bad value | `0x1A` | only a bad parameter number is listed |
| Data packet size (parameter 6) | 32/64/128/256 accepted | the parameter table allows 0..3, section 3.3.3 only 2 |
| Notepad page or index page out of range | `0x1A` | not listed |
| `ReadSysPara` status register | bit 1 last match/search passed, bit 2 password verified, bit 3 image in buffer | bit meaning from older GROW manuals (R303S); this one does not define them |
| Factory security level | 3 | not given |
| `GetFwVer`, `GetAlgVer`, `ReadInfPage` | emulator strings (`fp_emu-...`) | the real strings are unknown: a host must not depend on them |
| Processing time (`speed real`) | image 100 ms, features 200 ms, search 10 ms | the manual gives only "< 500 ms" and "< 10 ms per print" |

This module defeats biometrics on purpose: an external fingerprint module can be impersonated by
anything that speaks its UART. Lab and test boards only.

## Console (`lab_common`)

One command per line on the console UART; every reply is one line starting with `BENCH` (`BENCH OK ...` or `BENCH ERR ...`), so a host script can skip the log lines that share the port. Built in: `ping`. An unknown command answers with the list of the registered ones. A module adds its commands with `lab_console_register()`.
