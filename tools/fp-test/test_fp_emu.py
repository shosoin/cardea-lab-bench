#!/usr/bin/env python3
"""End-to-end tests of fp_emu (the bench's R502-F Pro emulator), driven like the key's firmware.

Needs the Pico 2 running tools/pico-fp-host (its GP8/GP9/GP10/GP11 wired to the S3's fp_emu
pins) and the bench S3's console, used to place fingers and inject faults.

    python3 test_fp_emu.py            # everything
    python3 test_fp_emu.py enroll     # only tests whose name contains "enroll"
    FP_SPEED=real python3 test_fp_emu.py   # with the emulator's processing delays

The tests reset the emulated module first (fp factory): its library and settings are lost.

SPDX-License-Identifier: AGPL-3.0-or-later
"""
import os
import sys
import time
import traceback

from r502 import Bench, BadChecksum, HostLink, R502, Timeout

link = HostLink()
bench = Bench()
fp = R502(link)
TESTS = []


def test(f):
    TESTS.append(f)
    return f


def eq(got, want, what=""):
    if got != want:
        raise AssertionError(f"{what}: got {got!r}, want {want!r}")


def power_cycle(off_s=0.05):
    link.power(False)
    time.sleep(off_s)
    link.flush()
    link.power(True)
    fp.wait_ready()


def no_answer(f, *a, timeout=0.6, **kw):
    try:
        f(*a, timeout=timeout, **kw)
    except Timeout:
        return
    raise AssertionError("expected no answer")


def setup():
    bench.fp("plug")
    bench.fp("power pin")
    bench.fp("lift")
    bench.fp("next clear")
    bench.fp("factory")
    bench.fp("opt speed " + os.environ.get("FP_SPEED", "fast"))
    bench.fp("opt timeout 10000")
    bench.fp("opt pwdgate 0")
    bench.fp("led clear")
    link.baud(57600)
    fp.addr, fp.pkt = 0xFFFFFFFF, 128
    power_cycle()


def enroll_manual(finger, slot, n=2):
    bench.fp(f"finger {finger}")
    for b in range(1, n + 1):
        eq(fp.get_image(), 0, "GetImage")
        eq(fp.gen_char(b), 0, f"GenChar {b}")
    eq(fp.reg_model(), 0, "RegModel")
    eq(fp.store(1, slot), 0, "StoreChar")
    bench.fp("lift")


def present(finger, q="good"):
    bench.fp(f"finger {finger} {q}")
    eq(fp.get_image(), 0, "GetImage")
    eq(fp.gen_char(1), 0, "GenChar")


# ------------------------------------------------------------------ power, lines, basics

@test
def power_and_ready_byte():
    link.power(False)
    time.sleep(0.1)
    eq(link.rxpin(), 0, "TX floats with the module off")
    link.flush()
    no_answer(fp.handshake, timeout=0.4)
    link.power(True)
    fp.wait_ready()
    eq(link.rxpin(), 1, "TX idles high with the module on")
    eq(fp.handshake(), 0, "HandShake")


@test
def wakeup_line():
    bench.fp("finger A")
    time.sleep(0.05)
    eq(link.wake(), 0, "WAKEUP with a finger")
    bench.fp("lift")
    time.sleep(0.05)
    eq(link.wake(), 1, "WAKEUP without a finger")
    link.power(False)
    bench.fp("finger A")
    time.sleep(0.05)
    eq(link.wake(), 0, "WAKEUP works with the main supply off")
    bench.fp("lift")
    bench.fp("unplug")
    time.sleep(0.05)
    eq(link.wake(), 1, "unplugged: WAKEUP floats high (no pulls since 2026-11-11, RP2350-E9) = no finger")
    bench.fp("plug")
    time.sleep(0.05)
    eq(link.wake(), 1, "plugged back")
    power_cycle()


@test
def sys_params_and_versions():
    c, p = fp.read_sys_para()
    eq(c, 0)
    eq((p["capacity"], p["addr"], p["pkt_code"], p["baud_n"], p["sysid"]), (1500, 0xFFFFFFFF, 2, 6, 600))
    eq(fp.alg_ver()[0], 0)
    eq(fp.fw_ver()[0], 0)
    c, page, n = fp.read_inf_page()
    eq((c, len(page), n), (0, 512, 4), "ReadInfPage: 512 bytes in 4 packets")
    c1, r1 = fp.random()
    c2, r2 = fp.random()
    eq((c1, c2), (0, 0))
    assert r1 != r2, "GetRandomCode twice the same"


@test
def protocol_errors():
    # bad checksum from the host -> 0x01
    pkt = bytearray(fp.packet(0x01, b"\x40"))
    pkt[-1] ^= 1
    link.tx(bytes(pkt))
    pid, p = fp.recv()
    eq(p[0], 0x01, "bad checksum")
    eq(fp.command(0x77)[0], 0xFC, "unknown command")
    eq(fp.command(0x01, b"\x00")[0], 0x01, "wrong length")
    no_answer(fp.command, 0x40, addr=0x12345678)   # other address: ignored
    eq(fp.handshake(), 0)


# ------------------------------------------------------------------ manual enrol / verify

@test
def capture_errors():
    eq(fp.get_image(), 0x02, "no finger")
    eq(fp.gen_char(1), 0x15, "GenChar with no image")
    bench.fp("finger A fail")
    eq(fp.get_image(), 0x03, "capture failed")
    bench.fp("finger A messy")
    eq(fp.get_image(), 0, "messy: image taken")
    eq(fp.gen_char(1), 0x06, "messy: no features")
    bench.fp("finger A small")
    eq(fp.get_image(), 0)
    eq(fp.gen_char(1), 0x07, "small: too few features")
    eq(fp.gen_char(7), 0x01, "buffer 7")
    bench.fp("lift")


@test
def enroll_and_search():
    enroll_manual("A", 5)
    enroll_manual("B", 7, n=4)
    eq(fp.template_num(), (0, 2))
    eq(fp.index_table(0), (0, {5, 7}))
    present("A")
    eq(fp.search(), (0, 5, 150), "A found in slot 5")
    present("B")
    eq(fp.search()[:2], (0, 7))
    eq(fp.search(1, 0, 7)[0], 0x09, "slot 7 outside the searched range")
    present("C")
    eq(fp.search(), (0x09, 0, 0), "C not enrolled")
    eq(fp.search(1, 1500, 1)[0], 0x0B, "start out of range")
    bench.fp("lift")


@test
def match_two_buffers():
    bench.fp("finger A")
    fp.get_image(); fp.gen_char(1)
    fp.get_image(); fp.gen_char(2)
    eq(fp.match(), (0, 150), "A vs A")
    bench.fp("finger B")
    fp.get_image(); fp.gen_char(2)
    eq(fp.match(), (0x08, 0), "A vs B")
    bench.fp("lift")
    # stored templates, as measured on the real module (2026-11-13)
    enroll_manual("A", 0)
    enroll_manual("A", 1)
    eq((fp.load(1, 0), fp.load(2, 0)), (0, 0))
    eq(fp.match(), (0, 409), "a template against itself: 409")
    eq(fp.load(2, 1), 0)
    eq(fp.match(), (0x08, 0), "two enrolments of the same finger: no match")
    present("A")
    eq(fp.load(2, 1), 0)
    eq(fp.match()[0], 0, "a fresh capture against its finger's template")


@test
def merge_different_fingers_fails():
    bench.fp("finger A")
    fp.get_image(); fp.gen_char(1)
    bench.fp("finger B")
    fp.get_image(); fp.gen_char(2)
    eq(fp.reg_model(), 0x0A, "A + B")
    bench.fp("lift")
    eq(fp.store(1, 1500), 0x0B, "StoreChar out of range")
    eq(fp.load(1, 3), 0x0C, "LoadChar of an empty slot")


@test
def ram_lost_at_power_off():
    bench.fp("finger A")
    fp.get_image()
    bench.fp("lift")
    power_cycle(0.05)
    eq(fp.gen_char(1), 0x15, "image buffer lost")
    eq(fp.up_char(1)[0], 0x0D, "char buffer lost")


@test
def template_transfers():
    enroll_manual("D", 3)
    eq(fp.load(2, 3), 0)
    c, tpl, n = fp.up_char(2)
    eq((c, len(tpl), n), (0, 512, 4), "UpChar")
    eq(fp.empty(), 0)
    eq(fp.down_char(1, tpl), 0, "DownChar")
    time.sleep(0.2)
    eq(fp.store(1, 9), 0, "store the downloaded template")
    present("D")
    eq(fp.search()[:2], (0, 9))
    # not a template: DownChar says 0x00 but drops it; the buffer keeps what it held (real module)
    eq(fp.load(1, 9), 0)
    eq(fp.down_char(1, bytes(range(256)) * 2), 0, "DownChar of foreign data still answers 0x00")
    time.sleep(0.2)
    eq(fp.up_char(1)[1], tpl, "foreign data dropped, buffer unchanged")
    eq(fp.store(1, 10), 0, "StoreChar stores the old buffer")
    eq(fp.load(2, 10), 0)
    eq(fp.match(), (0, 409), "slot 10 is a copy of slot 9")
    # a template with one byte changed: taken as sent, re-encoded on StoreChar
    bad = bytearray(tpl)
    bad[256] ^= 0xFF
    eq(fp.down_char(1, bytes(bad)), 0)
    time.sleep(0.2)
    eq(fp.up_char(1)[1], bytes(bad), "changed template taken as sent")
    eq(fp.store(1, 11), 0)
    eq(fp.load(1, 11), 0)
    c, back, _ = fp.up_char(1)
    eq(back != bytes(bad) and back == tpl, True, "re-encoded on StoreChar")
    bench.fp("lift")


@test
def image_transfers():
    enroll_manual("E", 0)
    bench.fp("finger E")
    eq(fp.get_image(), 0)
    bench.fp("lift")
    c, img, n = fp.up_image()
    eq((c, len(img), n), (0, 192 * 192 // 2, 144), "UpImage: 18432 bytes in 144 packets")
    power_cycle()
    eq(fp.down_image(img), 0, "DownImage")
    time.sleep(0.3)
    eq(fp.gen_char(1), 0, "features from the downloaded image")
    eq(fp.search()[:2], (0, 0))


@test
def delete_and_empty():
    for s in (1, 2, 3):
        enroll_manual("F", s)
    eq(fp.delete(2, 1), 0)
    eq(fp.index_table(0)[1], {1, 3})
    eq(fp.delete(1499, 2), 0x10, "past the end")
    eq(fp.empty(), 0)
    eq(fp.template_num(), (0, 0))


# ------------------------------------------------------------------ settings in "flash"

@test
def password():
    eq(fp.set_pwd(0x12345678), 0)
    power_cycle()
    eq(fp.handshake(), 0, "a password set gates nothing (real module)")
    eq(fp.read_notepad(0)[0], 0, "notepad readable without VfyPwd")
    for _ in range(20):
        eq(fp.vfy_pwd(1), 0x13, "wrong password")
    eq(fp.vfy_pwd(0x12345678), 0, "no lockout after 20 wrong ones")
    bench.fp("opt pwdgate 1")                 # a module that would require VfyPwd first
    power_cycle()
    eq(fp.handshake(), 0x21, "pwdgate: password required")
    eq(fp.vfy_pwd(0x12345678), 0)
    eq(fp.handshake(), 0)
    bench.fp("opt pwdgate 0")
    eq(fp.set_pwd(0), 0)
    power_cycle()
    eq(fp.vfy_pwd(0), 0, "no password again")


@test
def address():
    eq(fp.set_addr(0xAABBCCDD), 0)
    no_answer(fp.handshake)
    fp.addr = 0xAABBCCDD
    eq(fp.handshake(), 0)
    power_cycle()
    eq(fp.read_sys_para()[1]["addr"], 0xAABBCCDD, "address kept")
    eq(fp.set_addr(0xFFFFFFFF), 0)
    fp.addr = 0xFFFFFFFF
    eq(fp.handshake(), 0)


@test
def notepad():
    data = bytes(range(32))
    eq(fp.write_notepad(3, data), 0)
    power_cycle()
    eq(fp.read_notepad(3), (0, data))
    eq(fp.read_notepad(4), (0, bytes(32)))
    eq(fp.write_notepad(16, data), 0x1A, "page 16")


@test
def packet_size_and_baud():
    eq(fp.set_sys_para(6, 0), 0, "32-byte data packets")
    eq(fp.set_sys_para(5, 9), 0x1A, "security level 9")
    eq(fp.set_sys_para(8, 1), 0x1A, "parameter 8")
    eq(fp.set_sys_para(4, 12), 0, "115200 baud (answered at 57600)")
    link.baud(115200)
    time.sleep(0.05)
    link.flush()
    eq(fp.handshake(), 0, "115200 at once (real module)")
    power_cycle()
    c, page, n = fp.read_inf_page()
    eq((c, len(page), n), (0, 512, 16), "512 bytes in 32-byte packets")
    fp.set_sys_para(6, 2)
    fp.set_sys_para(4, 6)
    link.baud(57600)
    time.sleep(0.05)
    link.flush()
    eq(fp.handshake(), 0, "back to 57600 at once")
    power_cycle()


@test
def led_ring():
    eq(fp.aura(1, 0x50, 0x30, 0), 0)
    r = bench.fp("led")
    assert "host:breath/80/7col/0" in r, r


# ------------------------------------------------------------------ automatic operations

@test
def auto_enroll():
    bench.fp("tap A 6 300 300")
    fp.auto_enroll_start(steps=True, lift=True)
    got = [fp.auto_packet() for _ in range(15)]
    eq([p[1] for p in got], list(range(1, 16)), "steps 1..15")
    eq({p[0] for p in got}, {0}, "all ok")
    eq(got[-1][2], 0, "first free slot")
    eq(fp.template_num(), (0, 1))
    assert "module:on/0/green" in bench.fp("led"), "green at the end"


@test
def auto_enroll_errors():
    enroll_manual("A", 0)
    fp.auto_enroll_start(slot=0, overwrite=False)
    eq(fp.auto_packet(2)[0], 0x22, "slot used, no overwrite")
    bench.fp("finger A")
    fp.auto_enroll_start(slot=1, dup_ok=False, steps=False, lift=False)
    eq(fp.auto_packet(5)[:2], (0x27, 0x0D), "already enrolled (only the final packet)")
    bench.fp("finger B messy")
    fp.auto_enroll_start(slot=1, steps=False, lift=False)
    eq(fp.auto_packet(5)[:2], (0x07, 2), "no features")
    bench.fp("lift")
    bench.fp("opt timeout 800")
    fp.auto_enroll_start(slot=1, steps=False)
    eq(fp.auto_packet(5)[:2], (0x26, 1), "timeout with no finger")
    link.tx(fp.packet(0x01, bytes([0x31, 0, 1, 0, 0, 1])))   # one byte short: no answer
    no_answer(fp.recv, timeout=0.5)


@test
def auto_identify():
    enroll_manual("A", 4)
    bench.fp("finger A")
    fp.auto_identify_start(steps=True, max_err=1)
    got = [fp.auto_packet() for _ in range(3)]
    eq([p[:2] for p in got], [(0, 1), (0, 2), (0, 3)])
    eq(got[2][2:], (4, 150), "slot 4, score")
    bench.fp("finger B")
    fp.auto_identify_start(steps=False, max_err=1)
    eq(fp.auto_packet()[:2], (0x09, 3), "B not found")
    bench.fp("tap B 1 300")
    time.sleep(0.05)
    fp.auto_identify_start(steps=True, max_err=2)
    seen = [fp.auto_packet()[:2] for _ in range(3)]       # B: miss
    eq(seen[-1], (0x09, 3))
    time.sleep(0.5)                                       # B lifted: the next touch is a new attempt
    bench.fp("finger A")                                  # second attempt: A
    got = [fp.auto_packet() for _ in range(3)]
    eq(got[-1][:3], (0, 3, 4), "second attempt matches")
    bench.fp("lift")
    fp.auto_identify_start(grade=9)
    eq(fp.auto_packet(2)[0], 0x01, "security level 9")
    fp.empty()
    fp.auto_identify_start()
    eq(fp.auto_packet(2)[0], 0x24, "library empty")


@test
def cancel_and_busy():
    enroll_manual("A", 0)
    fp.auto_identify_start(steps=True)
    time.sleep(0.2)
    no_answer(fp.handshake, timeout=0.4)      # busy: ignored
    eq(fp.cancel(), 0, "Cancel")
    no_answer(fp.recv, timeout=0.5)
    eq(fp.handshake(), 0)


# ------------------------------------------------------------------ fault injection

@test
def faults():
    bench.fp("next GetImage code 03")
    eq(fp.get_image(), 0x03, "forced code")
    eq(fp.get_image(), 0x02, "only once")
    bench.fp("next any silent")
    no_answer(fp.handshake)
    bench.fp("next any badsum")
    try:
        fp.handshake()
        raise AssertionError("expected a bad checksum")
    except BadChecksum:
        pass
    bench.fp("next any short")
    no_answer(fp.handshake)
    link.flush()
    bench.fp("next any garbage")
    eq(fp.handshake(), 0, "noise before the answer is skipped")
    bench.fp("next any delay 700")
    no_answer(fp.handshake, timeout=0.4)
    eq(fp.recv(1)[1][0], 0, "late answer arrives")
    bench.fp("finger A")
    fp.get_image()
    bench.fp("next any reboot")
    link.flush()
    fp.send(1, b"\x40")
    fp.wait_ready()
    eq(fp.gen_char(1), 0x15, "after the reboot the image is gone")
    bench.fp("lift")


@test
def unplugged():
    bench.fp("unplug")
    time.sleep(0.05)
    no_answer(fp.handshake)
    eq(link.rxpin(), 0, "TX floats")
    link.flush()
    bench.fp("plug")
    fp.wait_ready()
    eq(fp.handshake(), 0)


def main(argv):
    sel = [t for t in TESTS if not argv or any(a in t.__name__ for a in argv)]
    passed = 0
    for t in sel:
        setup()
        try:
            t()
            passed += 1
            print(f"PASS {t.__name__}")
        except Exception as e:
            print(f"FAIL {t.__name__}: {e}")
            if "-v" in sys.argv:
                traceback.print_exc()
    stats = bench.fp("stats")
    print(f"{passed}/{len(sel)} passed   ({stats})")
    return 0 if passed == len(sel) else 1


if __name__ == "__main__":
    sys.exit(main([a for a in sys.argv[1:] if not a.startswith("-")]))
