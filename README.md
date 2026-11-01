# se05x-lab-bridge

A lab setup to develop firmware for the **NXP SE05x** secure elements (SE050/SE051) **without the chip**: your microcontroller talks real I2C to what it believes is the secure element, and a simulator answers.

```
Your MCU (unchanged firmware, real I2C driver, real T=1 stack)
   │  I2C, address 0x48, like the chip
ESP32-S3 bridge (relays bytes, knows nothing about crypto)
   │  Wi-Fi / TCP
SE05x simulator (T=1 + APDU applet: objects, policies, UserID, counters, ECC/EdDSA/RSA/AES)
```

The firmware under test cannot tell the difference at the protocol level, so the I2C driver, the T=1 stack (e.g. NXP Plug & Trust) and the APDU usage are all exercised. It is built for [Cardea Key](https://github.com/shosoin/cardea-key), but nothing here is specific to it.

> **This is a lab tool, not a security device.** The simulator keeps keys in a JSON file in clear, over an unauthenticated TCP socket. Use it on a trusted local network only.

## Status

| Part | State |
|---|---|
| `sim/` — simulator container | works |
| `bridge/` — ESP32-S3 I2C-target ↔ TCP bridge | works: 44/44 exchanges, 16-21 ms round trip over Wi-Fi |
| `tools/pico-probe/` — Pico / Pico 2 test program | works |

## The simulator (`sim/`)

The TCP server of wolfSSL's [SE050Sim](https://github.com/wolfSSL/simulators) (GPL-3.0-or-later), pinned to a commit, with our patches (`sim/patches/`):

- `0001`: optional persistence of the object store (`SE050_SIM_STORE`), so the simulated chip keeps its objects across restarts.
- `0002`: applet sessions (`CreateSession`, `ProcessSessionCmd`, `CloseSession`) with **ECKey authentication** (`ECKeySessionInternalAuthenticate` and the session's SCP03, also inside Platform SCP03); Authentication Objects with a persistent failed-attempt counter and a hard block at the maximum; policies evaluated per session owner, as on the real applet; `ALLOW_SIGN` / `ALLOW_VERIFY` checked for MACs; `ReadObjectAttributes` with the counter; the SE's ECKey session key pair (`0x7FFF0201`); TLV lengths in the `0x81 nn` form; `SE050_SIM_TRACE=1` logs every command after SCP03 unwrapping, with its status word.
- `0003`: T=1 retransmission (an R-block with an error indication gets the last response again; before, host and bridge waited forever) and store persistence off the command path (a background thread writes the latest snapshot; the synchronous write could take a second on a busy disk, longer than NXP's T=1 stack waits). It follows AN12543 and NXP Plug & Trust's host code; UserID and AESKey sessions are not modelled yet.

```sh
docker build -t se05x-sim sim/
docker run -d --name se05x-sim -p 8050:8050 -v se05x-sim-data:/data se05x-sim
```

On TCP port 8050 it speaks raw T=1-over-I2C frames: each request is one complete frame (`NAD PCB LEN payload CRC16`), and so is each response. A first check is the interface soft reset that NXP's stack sends at start-up, `5a cf 00 37 7f`, which returns a 35-byte ATR (`JCOP4 ATPO`).

## Testing the chain: `tools/pico-probe`

A tiny Pico / Pico 2 program that talks to the SE05x (or the bridge) exactly like NXP's T=1 stack: it sends the interface soft reset, polls for the `0xA5` start of frame, reads LEN and the rest, checks the CRC and prints the ATR on USB serial every 5 s. It recovers the I2C bus by itself after errors. Build it with the pico-sdk (`-DPICO_BOARD=pico2` for a Pico 2).

## Wiring (example: Raspberry Pi Pico 2)

| Pico 2 | ESP32-S3 |
|---|---|
| GP4 (I2C0 SDA, pin 6) | SDA (any GPIO, set in the bridge) |
| GP5 (I2C0 SCL, pin 7) | SCL |
| GND (pin 8) | GND |

Add 4.7 kΩ pull-ups from SDA and SCL to 3.3 V. Power each board from its own USB; only the grounds are joined. The same pins can later connect a real SE05x board.

## Limitations

- It simulates an **SE050**, not exactly an **SE051**. Applet versions, supported curves and timing differ; validate on a real chip before trusting results.
- Not every SE05x command is implemented (see SE050Sim's README); operations like HKDF, HMAC or PBKDF2 may need adding.
- The Wi-Fi hop adds latency. The real chip also answers "not ready" (I2C NACK) while it computes and the host retries, so the bridge does the same while waiting for the simulator.

## License

GPL-3.0-or-later, the license of the simulator it builds on.
