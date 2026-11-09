# Fingerprint module tests (`fp-test` + `pico-fp-host`)

End-to-end tests of the bench's R502-F Pro emulator (`fp_emu`), driven the way the key's firmware
will drive the module, plus a reference driver for the protocol.

```
PC: test_fp_emu.py / r502.py
 ├─ USB serial ─ Pico 2 running tools/pico-fp-host ─ UART1 GP8/GP9, WAKEUP GP10, power GP11 ─┐
 └─ USB serial ─ ESP32-S3 console ("fp finger A", "fp next ...")                    fp_emu ─┘
```

The Pico uses the same GPIOs as the Cardea Key PCB, so the wiring is the one the key firmware will
use (`firmware/README.md`, module `fp_emu`).

## Pico test host (`tools/pico-fp-host`)

```sh
cd tools/pico-fp-host && mkdir -p build && cd build
cmake -DPICO_SDK_PATH=/path/to/pico-sdk ..   # Pico 2 by default; -DPICO_BOARD=pico for an RP2040
make                                         # fp_host.uf2
```

It writes nothing to flash beyond its own image (about 32 KB at the start), so data kept by other
firmware at the end of the flash survives; the firmware it replaces has to be flashed back
afterwards. USB serial commands: `ping`, `baud N`, `pwr 0|1` (1 = module powered, GP11 low),
`tx <hex>`, `wake`, `rxpin`; it sends `RX <hex>` with the module's bytes and `WAKE <0|1>` on
changes. `rxpin` reads GP9 against a pull-down with the input buffer off while it settles
(RP2350-E9), to tell a floating TX (module off) from an idle one.

## Tests

```sh
pip install pyserial
python3 tools/fp-test/test_fp_emu.py               # 23 tests, about 70 s
python3 tools/fp-test/test_fp_emu.py enroll        # only names containing "enroll"
FP_SPEED=real python3 tools/fp-test/test_fp_emu.py # with the emulator's processing delays
```

Ports are found by USB ID (Pico `2e8a:0009`/`000a`, S3 `1a86:55d3`/`303a:1001`) or with
`FP_HOST_PORT` / `CARDEA_BENCH_PORT`. Each test starts with `fp factory`: the emulated library and
settings are erased.

Covered: power-on `0x55`, TX floating with the module off, WAKEUP (also with the main supply off
and unplugged), system parameters, versions, info page, random numbers, protocol errors (bad
checksum, unknown command, wrong length, other address), capture errors, manual enrolment and
search, `Match`, merge failures, RAM lost at power-off, template and image transfers both ways,
delete / empty, password, address, notepad, packet size and 115200 baud, LED ring, `AutoEnroll`
(steps and errors), `AutoIdentify` (match, miss, retry, errors), `Cancel`, fault injection and an
unplugged module.

## Against the real module

`r502.py` only needs the Pico: with an R502-F Pro wired to the same Pico pins (VCC and 3.3VT to
3V3, since the Pico has no power switch), `HostLink` + `R502` drive the real module. Tests that
place fingers or inject faults need the emulator's console; the rest is the checklist for the
choices marked `UNVERIFIED` in `firmware/README.md`.
