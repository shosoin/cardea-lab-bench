# se05x-lab-bridge

An I2C bridge for **[SE050Sim](https://github.com/wolfSSL/simulators)**, wolfSSL's simulator of the **NXP SE05x** secure elements (SE050/SE051), so that firmware can be developed **without the chip**: your microcontroller talks real I2C to what it believes is the secure element, and SE050Sim answers. Many thanks to wolfSSL for publishing it.

```
Your MCU (unchanged firmware, real I2C driver, real T=1 stack)
   │  I2C, address 0x48, like the chip
ESP32-S3 bridge (relays frames, knows nothing about crypto)
   │  Wi-Fi / TCP
SE050Sim TCP server
```

The bridge relays raw T=1 frames between the I2C bus and a TCP socket and knows nothing about the applet. The firmware under test cannot tell the difference at the protocol level, so the I2C driver, the T=1 stack (e.g. NXP Plug & Trust) and the APDU usage are all exercised. It was built for [Cardea Key](https://github.com/shosoin/cardea-key), but nothing here is specific to it.

> **This is a lab tool, not a security device.** The simulator keeps keys in clear, behind an unauthenticated TCP socket. Use it on a trusted local network only.

## Status

| Part | State |
|---|---|
| `bridge/` — ESP32-S3 I2C-target ↔ TCP bridge | works: 16-21 ms round trip over Wi-Fi; recovers by itself when the simulator restarts |
| `tools/pico-probe/` — Pico / Pico 2 test program | works |
| `sim/` — container recipe for the simulator | works |

## The bridge (`bridge/`)

ESP-IDF 5.5 firmware for an ESP32-S3 board. It acts as an I2C target at address `0x48` (SDA GPIO8, SCL GPIO9 by default), joins your Wi-Fi and keeps a TCP connection to the simulator. Each T=1 frame the host writes is sent to the simulator, and its answer is served on the host's next reads.

Copy `bridge/config.local.h.example` to `bridge/config.local.h`, fill in your Wi-Fi and the simulator's address, then `idf.py build flash` from `bridge/`.

Behaviour worth knowing:

- While the simulator has not answered (or is unreachable), the bridge has no frame to serve: the host reads no `0xA5` start byte and polls again, as it does while a real chip is busy. A simulator that does not answer within 2 s is dropped and reconnected on the next frame, so the host's own retries recover the link after a simulator restart.
- ESP32-S3 I2C-target quirks it works around: SCL is stretched on every read address match until software releases it; a write only goes straight into the hardware FIFO if it all fits (so the first chunk is kept small); one byte is dropped after a 2-byte read (a filler byte is inserted after the header); the peripheral is re-created for each request.
- Each frame the host writes is checked (length against LEN, and CRC) before it is relayed. A damaged one, e.g. a write cut short when the receive FIFO is not drained in time while Wi-Fi is busy, is dropped and logged; the host gets no answer and retransmits, as it would after a bus error with a real chip.
- A watchdog re-creates the I2C target if SCL stays low for 1.5 s (e.g. the host was reset or re-flashed in the middle of a read).
- The RGB LED stays off and only flashes to signal events (green: connected; red: simulator unreachable; amber: I2C watchdog).

## Testing the chain: `tools/pico-probe`

A tiny Pico / Pico 2 program that talks to the SE05x (or the bridge) exactly like NXP's T=1 stack: it sends the interface soft reset, polls for the `0xA5` start of frame, reads LEN and the rest, checks the CRC and prints the ATR on USB serial every 5 s. It recovers the I2C bus by itself after errors. Build it with the pico-sdk (`-DPICO_BOARD=pico2` for a Pico 2).

## Wiring (example: Raspberry Pi Pico 2)

| Pico 2 | ESP32-S3 |
|---|---|
| GP4 (I2C0 SDA, pin 6) | SDA (GPIO8 by default) |
| GP5 (I2C0 SCL, pin 7) | SCL (GPIO9 by default) |
| GND (pin 8) | GND |

Wire them pin to pin with short wires: both ends enable their internal pull-ups (the bridge does, and so do `pico-probe` and the Pico firmware we test), which is enough at 100 kHz. For longer wires, add 4.7 kΩ pull-ups from SDA and SCL to 3.3 V. Power each board from its own USB; only the grounds are joined.

## The simulator container (`sim/`)

A convenience recipe that builds SE050Sim's TCP server from the repository and commit set in `sim/Dockerfile`, with an optional persistent object store:

```sh
docker build -t se05x-sim sim/
docker run -d --name se05x-sim -p 8050:8050 -v se05x-sim-data:/data se05x-sim
```

On TCP port 8050 it speaks raw T=1-over-I2C frames: each request is one complete frame (`NAD PCB LEN payload CRC16`), and so is each response. A first check is the interface soft reset that NXP's stack sends at start-up, `5a cf 00 37 7f`, which returns a 35-byte ATR (`JCOP4 ATPO`). See [SE050Sim](https://github.com/wolfSSL/simulators) for what it implements and how to configure it.

## Limitations

- It is a simulator: applet versions, supported features and timing can differ from a real SE050/SE051. Validate on a real chip before trusting results.
- The Wi-Fi hop adds latency.

## License

AGPL-3.0-or-later. SE050Sim, which the container builds, is wolfSSL's and keeps its own license (GPL-3.0-or-later).
