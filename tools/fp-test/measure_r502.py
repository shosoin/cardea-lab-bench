#!/usr/bin/env python3
"""One-session measurement battery for the GROW R502-F Pro (cardea-lab-bench#5, cardea-key-dev#42).

Measures, in one go, what the Cardea Key's sensor hardening needs to know about the real module
(identity, notepad, templates, Match determinism, DownChar, 115200 baud, password behaviour), and
writes a JSON report. Same code against the bench's emulator (fp_emu, fingers placed from the S3
console) and against a real module wired to the Pico (fingers placed by a person, prompted on
screen; the module's own LED ring says when to touch and lift).

    python3 measure_r502.py --emu                 # dry run against fp_emu, fully automatic
    python3 measure_r502.py --real --wipe         # the real module (its library is emptied)
    python3 measure_r502.py --real --only identity   # read-only: identity + notepad (second unit)

Needs the Pico 2 running tools/pico-fp-host. With --real the module is powered from the Pico's
3V3 (no switch on the bench), so the two power cycles are done by unplugging the Pico's USB.

Only documented commands are sent (no scanning of unknown opcodes). SetAddr is never used. The
only password ever set is TEST_PWD below, printed and written to the report before it is set; it
is put back to 0 at the end (also in the clean-up after an error).

Templates are biometric data: the report keeps only their sizes and SHA-256, never their bytes.

SPDX-License-Identifier: AGPL-3.0-or-later
"""
import argparse
import hashlib
import itertools
import json
import os
import subprocess
import sys
import time
import traceback

from r502 import CODES, HOST_IDS, BadChecksum, Bench, HostLink, R502, Timeout, find_port

TEST_PWD = 0xCA4DEA01            # the only password this script ever sets
BAUDS = (57600, 115200, 9600, 19200, 38400)
L_BREATH, L_FLASH, L_ON, L_OFF = 1, 2, 3, 4
RED, BLUE, PURPLE, GREEN, YELLOW = 1, 2, 3, 4, 5
FINGERS = {"A": "dedo A", "B": "dedo B"}
ZONES_B = ("centro", "borde izquierdo", "borde derecho", "punta")
SOUNDS = "/usr/share/sounds/freedesktop/stereo/"


def sha(b):
    return hashlib.sha256(b).hexdigest()


def code_name(c):
    return f"0x{c:02X} {CODES.get(c, '?')}"


class Report:
    def __init__(self, path, meta):
        self.path = path
        self.d = {"meta": meta, "sections": {}, "errors": []}
        self.save()

    def sec(self, name):
        return self.d["sections"].setdefault(name, {})

    def error(self, where, e):
        self.d["errors"].append({"where": where, "error": repr(e), "trace": traceback.format_exc()})
        self.save()

    def save(self):
        tmp = self.path + ".tmp"
        with open(tmp, "w") as f:
            json.dump(self.d, f, indent=1, ensure_ascii=False)
        os.replace(tmp, self.path)


# ------------------------------------------------------------------ who places the fingers

class EmuFingers:
    """fp_emu: fingers and power from the S3 console and the Pico's GP11."""
    real = False

    def __init__(self, m):
        self.m = m
        self.bench = Bench()

    def fp(self, line):
        for attempt in (1, 2):                       # the console can lag behind an NVS write
            try:
                return self.bench.cmd("fp " + line, timeout=15)
            except Timeout:
                if attempt == 2:
                    raise

    def setup(self, speed):
        for c in ("plug", "power pin", "lift", "next clear", "factory", f"opt speed {speed}",
                  "opt timeout 10000", "led clear"):
            self.fp(c)

    def place(self, finger, hint):
        self.fp(f"finger {finger}")

    def lift(self):
        self.fp("lift")

    def say(self, title, text, popup=False):
        print(f"  [{title}] {text}", flush=True)

    def power_cycle(self, why):
        self.m.link.power(False)
        time.sleep(0.3)
        self.m.link.flush()
        self.m.link.power(True)
        time.sleep(0.5)               # 0x55 comes at the new baud rate if it changed: just wait
        self.m.link.flush()


class Person:
    """A real module: a person touches it, prompted on the terminal, by the ring and a popup."""
    real = True

    def __init__(self, m):
        self.m = m

    def setup(self, speed):
        pass

    def say(self, title, text, popup=False, error=False):
        print(f"\n>>> {title}: {text}\n", flush=True)
        if popup:
            env = dict(os.environ, DISPLAY=os.environ.get("DISPLAY", ":0.0"))
            snd = SOUNDS + ("dialog-warning.oga" if error else "complete.oga")
            subprocess.Popen(["sh", "-c", f"for i in 1 2 3; do paplay {snd}; done"], env=env,
                             stderr=subprocess.DEVNULL)
            subprocess.Popen(["zenity", "--error" if error else "--info", f"--title={title}",
                              f"--text={text}", "--width=420", "--timeout=60"], env=env,
                             stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)

    def place(self, finger, hint):
        print(f"    toca con el {FINGERS[finger]}{' (' + hint + ')' if hint else ''}  [anillo azul]",
              flush=True)
        subprocess.Popen(["paplay", SOUNDS + "message.oga"], stderr=subprocess.DEVNULL)

    def lift(self):
        print("    levanta el dedo", flush=True)

    def power_cycle(self, why):
        self.say("Cardea: apagar y encender el sensor",
                 f"{why}\nDesenchufa el USB de la Pico 2 (la de la llave), espera 3 s y vuelve a "
                 "enchufarlo. El script sigue solo.", popup=True)
        m = self.m
        m.link.alive = False
        t0 = time.time()
        while time.time() - t0 < 300:              # wait for the port to go...
            if not any((p.vid, p.pid) in HOST_IDS for p in _ports()):
                break
            time.sleep(0.2)
        else:
            raise RuntimeError("the Pico was not unplugged in 5 minutes")
        try:
            m.link.s.close()
        except Exception:                          # noqa: BLE001 - the port is gone anyway
            pass
        print("    desenchufada; esperando a que vuelva...", flush=True)
        while time.time() - t0 < 600:              # ...and come back
            if any((p.vid, p.pid) in HOST_IDS for p in _ports()):
                time.sleep(1.5)
                m.link = HostLink()
                m.fp.link = m.link
                time.sleep(0.5)
                m.link.flush()
                print("    de vuelta", flush=True)
                return
            time.sleep(0.2)
        raise RuntimeError("the Pico did not come back in 10 minutes")


def _ports():
    import serial.tools.list_ports
    return serial.tools.list_ports.comports()


# ------------------------------------------------------------------ measurement session

class Session:
    def __init__(self, a):
        self.a = a
        self.link = HostLink()
        self.fp = R502(self.link)
        self.who = Person(self) if a.real else EmuFingers(self)
        self.baud = 57600
        self.pwd_set = False
        self.baud_changed = False
        self.tpl = {}                  # slot -> template bytes (memory only, never in the report)
        stamp = time.strftime("%Y%m%d-%H%M%S")
        self.r = Report(a.out or f"r502-report-{'real' if a.real else 'emu'}-{stamp}.json", {
            "mode": "real" if a.real else "emu", "started": time.strftime("%Y-%m-%dT%H:%M:%S%z"),
            "test_password": f"0x{TEST_PWD:08X}", "repeats": a.repeats, "script": "measure_r502.py"})
        print(f"report: {self.r.path}\ntest password (the only one this script sets): 0x{TEST_PWD:08X}")

    # -- helpers
    def timed(self, f, *args, **kw):
        t0 = time.perf_counter()
        out = f(*args, **kw)
        return out, round((time.perf_counter() - t0) * 1000, 1)

    def answer(self, f, *args, **kw):
        """Confirmation code of a command, or 'timeout' / 'badsum'."""
        try:
            out = f(*args, **kw)
            return out if isinstance(out, int) else out[0]
        except Timeout:
            return "timeout"
        except BadChecksum:
            return "badsum"
        except Exception as e:                       # noqa: BLE001 - e.g. a short answer
            return f"exception {e!r}"

    def find_baud(self, prefer=None):
        for b in ([prefer] if prefer else []) + [self.baud] + list(BAUDS):
            self.link.baud(b)
            time.sleep(0.05)
            self.link.flush()
            try:
                self.fp.handshake(timeout=0.5)     # any well-formed answer (also 0x21) will do
                self.baud = b
                return b
            except (Timeout, BadChecksum, RuntimeError):
                continue
        raise RuntimeError("the module answers at no baud rate")

    def ring(self, ctrl, color, speed=0x40, times=0):
        if self.who.real:
            try:
                self.fp.aura(ctrl, speed, color, times)
            except (Timeout, BadChecksum):
                pass

    def capture(self, buf, finger, hint, log):
        """One touch into char buffer buf: (GetImage, GenChar) until it works."""
        for attempt in range(6):
            self.ring(L_BREATH, BLUE)
            self.who.place(finger, hint)
            t0 = time.time()
            while True:
                c = self.fp.get_image()
                if c == 0:
                    break
                if c != 0x02:
                    log.append({"get_image": code_name(c)})
                if time.time() - t0 > 90:
                    raise RuntimeError("no finger in 90 s")
                time.sleep(0.05)
            c, ms = self.timed(self.fp.gen_char, buf)
            if c == 0:
                self.ring(L_FLASH, GREEN, 0x20, 1)
                log.append({"gen_char_ms": ms})
            else:
                self.ring(L_FLASH, RED, 0x20, 1)
                log.append({"gen_char": code_name(c), "attempt": attempt})
            self.who.lift()
            self.wait_lift()
            if c == 0:
                return
        raise RuntimeError("six bad touches in a row")

    def wait_lift(self):
        t0 = time.time()
        while self.fp.get_image() != 0x02:
            if time.time() - t0 > 60:
                raise RuntimeError("finger not lifted in 60 s")
            time.sleep(0.05)

    def enroll(self, finger, slot, hint, log):
        for b in range(1, 5):
            self.capture(b, finger, hint, log)
        c, ms = self.timed(self.fp.reg_model)
        log.append({"reg_model": code_name(c), "ms": ms})
        if c != 0:
            return c
        c = self.fp.store(1, slot)
        log.append({"store": code_name(c), "slot": slot})
        self.ring(L_FLASH, PURPLE, 0x20, 2)
        return c

    def up_slot(self, slot):
        c = self.fp.load(1, slot)
        if c != 0:
            return c, b"", 0, 0
        (c, data, n), ms = self.timed(self.fp.up_char, 1)
        return c, data, n, ms

    def match_pair(self, i, j, repeats):
        c1, c2 = self.fp.load(1, i), self.fp.load(2, j)
        if c1 or c2:
            return {"load": [code_name(c1), code_name(c2)]}
        scores, codes, times = [], set(), []
        for _ in range(repeats):
            (c, s), ms = self.timed(self.fp.match)
            scores.append(s)
            codes.add(c)
            times.append(ms)
        return {"scores": scores, "codes": sorted(code_name(c) for c in codes),
                "deterministic": len(set(scores)) == 1, "ms_avg": round(sum(times) / len(times), 1)}

    def section(self, name, f):
        print(f"\n== {name}", flush=True)
        try:
            f(self.r.sec(name))
        except Exception as e:                       # noqa: BLE001 - record and go on
            print(f"   ERROR in {name}: {e!r}", flush=True)
            self.r.error(name, e)
            if isinstance(e, (KeyboardInterrupt, SystemExit)):
                raise
        self.r.save()

    # -- sections
    def s_state(self, s):
        s["baud"] = self.find_baud()
        c, sp = self.fp.read_sys_para()
        s["sys_para"] = {"code": code_name(c), **sp}
        if sp.get("pkt_code") is not None and c == 0:
            self.fp.pkt = 32 << sp["pkt_code"]
        c, n = self.fp.template_num()
        s["template_num"] = n
        used = set()
        for page in range(6):                        # 1500 slots, 256 per page
            c, u = self.fp.index_table(page)
            used |= u
        s["used_slots"] = sorted(used)
        self.notepad0 = {}
        for p in range(16):
            c, d = self.fp.read_notepad(p)
            self.notepad0[p] = d
        s["notepad_nonzero_pages"] = [p for p, d in self.notepad0.items() if any(d)]
        print(f"   baud {s['baud']}, {n} templates in {sorted(used)[:12]}{'...' if len(used) > 12 else ''}")

    def s_identity(self, s):
        c, sp = self.fp.read_sys_para()
        s["sys_para"] = {"code": code_name(c), **sp}
        (c, data, n), ms = self.timed(self.fp.read_inf_page)
        s["inf_page"] = {"code": code_name(c), "len": len(data), "packets": n, "ms": ms,
                         "sha256": sha(data), "hex": data.hex(),
                         "ascii": "".join(chr(b) if 32 <= b < 127 else "." for b in data)}
        s["fw_ver"] = self.fp.fw_ver()[1]
        s["alg_ver"] = self.fp.alg_ver()[1]
        s["random"] = [f"{self.fp.random()[1]:08X}" for _ in range(5)]
        print(f"   fw {s['fw_ver']!r} alg {s['alg_ver']!r}, info page {len(data)} B")

    def s_notepad(self, s):
        self.pattern = b"CARDEA-R502-MEASURE-" + os.urandom(6).hex().encode()[:12]
        s["pattern"] = self.pattern.decode()
        s["write"] = code_name(self.fp.write_notepad(15, self.pattern))
        c, d = self.fp.read_notepad(15)
        s["read_back_equal"] = d == self.pattern
        s["page15_before"] = self.notepad0.get(15, b"").hex()

    def s_enroll(self, s):
        if s.get("done"):
            return
        c = self.fp.empty()
        s["empty_before"] = code_name(c)
        self.who.say("Cardea: alta de dos dedos",
                     "16 toques con el dedo A (moviéndolo un poco entre toques) y luego 16 con el "
                     "dedo B por zonas (centro, borde izquierdo, borde derecho, punta). Toca cuando "
                     "el anillo esté azul y levanta al ver el verde.", popup=True)
        for k in range(4):
            log = s.setdefault(f"A_slot{k}", [])
            self.enroll("A", k, "mezclado", log)
        for k in range(4):
            log = s.setdefault(f"B_slot{4 + k}", [])
            self.who.say("zona", f"dedo B, plantilla {k + 1}: {ZONES_B[k]}")
            self.enroll("B", 4 + k, ZONES_B[k], log)
        s["template_num"] = self.fp.template_num()[1]
        s["done"] = True

    def s_templates(self, s, tag="before"):
        for slot in range(8):
            reps = []
            for _ in range(3):
                c, data, n, ms = self.up_slot(slot)
                reps.append((c, data, n, ms))
            c, data, n, ms = reps[0]
            if tag == "before":
                self.tpl[slot] = data
            s[f"slot{slot}"] = {"code": code_name(c), "len": len(data), "packets": n,
                                "ms": [r[3] for r in reps], "sha256": sha(data),
                                "identical_x3": all(r[1] == data for r in reps),
                                **({"same_as_before_power_cycle": data == self.tpl.get(slot)}
                                   if tag != "before" else {})}
        print(f"   template {s['slot0']['len']} B, UpChar {s['slot0']['ms']} ms, "
              f"identical x3: {all(s[f'slot{k}']['identical_x3'] for k in range(8))}")

    def s_match(self, s, repeats):
        for i, j in itertools.combinations_with_replacement(range(8), 2):
            s[f"{i}-{j}"] = self.match_pair(i, j, repeats)
        s["1-0_swapped"] = self.match_pair(1, 0, repeats)
        det = all(v.get("deterministic") for v in s.values() if isinstance(v, dict) and "scores" in v)
        s["all_deterministic"] = det
        print(f"   all deterministic: {det}; self 0-0 {s['0-0'].get('scores', [None])[0]}, "
              f"A-A 0-1 {s['0-1'].get('scores', [None])[0]}, A-B 0-4 {s['0-4'].get('scores', [None])[0]}")

    def s_downchar(self, s):
        a0 = self.tpl.get(0, b"")
        if not a0:
            raise RuntimeError("no template of slot 0")
        # (a) a real template uploaded, downloaded into slot 100: byte-identical? matches finger A?
        s["real_down"] = code_name(self.fp.down_char(1, a0))
        s["real_store_100"] = code_name(self.fp.store(1, 100))
        c, data, n, ms = self.up_slot(100)
        s["real_100_identical"] = data == a0
        self.who.say("Cardea: un toque con el dedo A", "Para ver si una plantilla copiada vale.",
                     popup=True)
        log = []
        self.capture(1, "A", "", log)
        c, page, score = self.fp.search(1, 100, 1)
        s["finger_A_vs_copy_at_100"] = {"code": code_name(c), "page": page, "score": score}
        c, page, score = self.fp.search(1, 0, 80)
        s["finger_A_vs_0_79"] = {"code": code_name(c), "page": page, "score": score}
        c, page, score = self.fp.search(1, 0, 1500)
        s["finger_A_vs_all"] = {"code": code_name(c), "page": page, "score": score}
        s["touch_log"] = log
        # (b) one byte flipped in the middle: is there a checksum / format check?
        bad = bytearray(a0)
        bad[len(bad) // 2] ^= 0xFF
        s["flipped_down"] = code_name(self.fp.down_char(1, bytes(bad)))
        s["flipped_store_101"] = code_name(self.fp.store(1, 101))
        c, data, n, ms = self.up_slot(101)
        s["flipped_101_identical"] = data == bytes(bad) if c == 0 else code_name(c)
        # (c) arbitrary bytes of the same length: a "virtual template"?
        rnd = os.urandom(len(a0))
        s["random_down"] = code_name(self.fp.down_char(1, rnd))
        s["random_store_102"] = code_name(self.fp.store(1, 102))
        c, data, n, ms = self.up_slot(102)
        s["random_102_identical"] = data == rnd if c == 0 else code_name(c)
        c = self.fp.load(1, 0)
        c2 = self.fp.load(2, 102)
        s["random_vs_A0_match"] = self.fp.match() if not (c or c2) else [code_name(c), code_name(c2)]
        for slot in (100, 101, 102):
            self.fp.delete(slot)
        s["used_after"] = sorted(self.fp.index_table(0)[1])

    def baud_measure(self, s):
        c, data, n, ms = self.up_slot(0)
        s["at"] = self.baud
        s["up_char_ms"] = ms
        s["up_char_identical"] = data == self.tpl.get(0)
        s["match_0_1"] = self.match_pair(0, 1, 20)

    def s_baud(self, s):
        self.baud_measure(s.setdefault("57600", {}))
        s["set_115200"] = code_name(self.fp.set_sys_para(4, 12))
        self.baud_changed = True
        b = self.find_baud(prefer=57600)
        s["right_after_set"] = b
        s["applies"] = "at once" if b == 115200 else "next power-on"
        if b == 115200:
            self.link.baud(115200)
            self.baud_measure(s.setdefault("115200", {}))
            s["set_back"] = code_name(self.fp.set_sys_para(4, 6))
            s["right_after_set_back"] = self.find_baud(prefer=115200)
            self.baud_changed = self.baud != 57600

    def s_password_set(self, s):
        s["password"] = f"0x{TEST_PWD:08X}"
        self.r.save()
        print(f"   setting the test password 0x{TEST_PWD:08X}", flush=True)
        c = self.fp.set_pwd(TEST_PWD)
        s["set_pwd"] = code_name(c)
        self.pwd_set = c == 0
        s["same_power_on"] = {"handshake": self.answer(self.fp.handshake),
                              "template_num": self.answer(self.fp.template_num)}

    def no_pwd_probe(self):
        f = self.fp
        out = {k: self.answer(g) for k, g in (
            ("handshake", f.handshake), ("read_sys_para", f.read_sys_para), ("fw_ver", f.fw_ver),
            ("alg_ver", f.alg_ver), ("read_inf_page", f.read_inf_page),
            ("read_notepad", lambda: f.read_notepad(15)), ("template_num", f.template_num),
            ("index_table", f.index_table), ("random", f.random), ("load_char", lambda: f.load(1, 0)),
            ("aura", lambda: f.aura(L_OFF, 0, 0, 0)))}
        return {k: code_name(v) if isinstance(v, int) else v for k, v in out.items()}

    def s_after_cycle(self, s):
        s["baud"] = self.find_baud()
        s["without_vfy_pwd"] = self.no_pwd_probe()
        print(f"   without VfyPwd: {s['without_vfy_pwd']}")
        s["vfy_pwd"] = code_name(self.fp.vfy_pwd(TEST_PWD))
        c, d = self.fp.read_notepad(15)
        s["notepad_kept"] = d == self.pattern
        if s.get("baud") == 115200 and self.r.sec("baud").get("applies") == "next power-on":
            self.baud_measure(self.r.sec("baud").setdefault("115200", {}))
            self.r.sec("baud")["set_back"] = code_name(self.fp.set_sys_para(4, 6))

    def s_password_tries(self, s):
        wrong = [(TEST_PWD ^ (0x1111 * (k + 1))) & 0xFFFFFFFF for k in range(60)]
        first = []
        for w in wrong[:5]:
            c, ms = self.timed(self.fp.vfy_pwd, w)
            first.append({"code": code_name(c), "ms": ms})
        s["wrong_5"] = first
        s["right_after_5"] = code_name(self.fp.vfy_pwd(TEST_PWD))
        batches = []
        for b in range(5):
            times, codes = [], set()
            for w in wrong[5 + 10 * b:15 + 10 * b]:
                c, ms = self.timed(self.answer, self.fp.vfy_pwd, w)
                times.append(ms)
                codes.add(c if isinstance(c, str) else code_name(c))
            c = self.answer(self.fp.vfy_pwd, TEST_PWD)
            batches.append({"wrong_ms_avg": round(sum(times) / len(times), 1),
                            "wrong_ms_max": max(times), "codes": sorted(codes),
                            "right_after": c if isinstance(c, str) else code_name(c)})
            self.r.save()
            if c != 0:
                s["lockout"] = f"right password refused after {15 + 10 * b} wrong ones"
                for wait in (5, 30, 60):
                    time.sleep(wait)
                    c = self.answer(self.fp.vfy_pwd, TEST_PWD)
                    s.setdefault("retry_right", []).append({"after_s": wait, "code": str(c)})
                    if c == 0:
                        break
                break
        s["batches_of_10"] = batches

    def s_password_clear(self, s):
        s["vfy_pwd"] = code_name(self.fp.vfy_pwd(TEST_PWD))
        c = self.fp.set_pwd(0)
        s["set_pwd_0"] = code_name(c)
        if c == 0:
            self.pwd_set = False
        s["vfy_pwd_0"] = code_name(self.fp.vfy_pwd(0))

    def s_empty(self, s):
        s["empty"] = code_name(self.fp.empty())
        s["template_num_after"] = self.fp.template_num()[1]
        c, d = self.fp.read_notepad(15)
        s["empty_clears_notepad"] = d != self.pattern
        s["notepad_page15_after_empty"] = d.hex()

    def s_cleanup(self, s):
        s["notepad_restore"] = code_name(self.fp.write_notepad(15, self.notepad0.get(15, bytes(32))))
        if self.baud_changed or self.baud != 57600:
            s["baud_restore"] = code_name(self.fp.set_sys_para(4, 6))
        if self.a.leave_b_at_100 and self.tpl.get(4):
            # finger B's centre template, downloaded after Empty: the key must deny it (#40)
            s["B_down"] = code_name(self.fp.down_char(1, self.tpl[4]))
            s["B_store_100"] = code_name(self.fp.store(1, 100))
        self.ring(L_OFF, 0)

    def s_final(self, s):
        s["baud"] = self.find_baud(prefer=57600)
        s["without_vfy_pwd"] = self.no_pwd_probe()
        c, sp = self.fp.read_sys_para()
        s["sys_para"] = {"code": code_name(c), **sp}
        s["template_num"] = self.fp.template_num()[1]
        s["used_slots"] = sorted(self.fp.index_table(0)[1])
        c, d = self.fp.read_notepad(15)
        s["notepad15_restored"] = d == self.notepad0.get(15, bytes(32))
        ok = (s["baud"] == 57600 and s["without_vfy_pwd"]["template_num"].startswith("0x00"))
        s["module_as_new"] = ok
        print(f"   baud {s['baud']}, no password needed: {ok}, templates in {s['used_slots']}")

    # -- emergency clean-up: never leave the module with a password or another baud rate
    def rescue(self):
        print("\n!! clean-up after an error", flush=True)
        try:
            self.find_baud()
            if self.pwd_set:
                print(f"   VfyPwd 0x{TEST_PWD:08X} + SetPwd 0", flush=True)
                self.fp.vfy_pwd(TEST_PWD)
                if self.fp.set_pwd(0) == 0:
                    self.pwd_set = False
            if self.baud != 57600 or self.baud_changed:
                self.fp.set_sys_para(4, 6)
        except Exception as e:                      # noqa: BLE001
            print(f"   clean-up failed: {e!r}", flush=True)
        if self.pwd_set:
            print(f"\n!!! THE MODULE MAY STILL HAVE THE TEST PASSWORD 0x{TEST_PWD:08X} !!!\n")
        self.r.d["meta"]["rescue_pwd_left"] = self.pwd_set
        self.r.save()

    def run(self):
        a = self.a
        self.who.setup(a.speed)
        if not self.who.real:
            self.who.power_cycle("start")
        t0 = time.time()
        self.section("state", self.s_state)
        self.section("identity", self.s_identity)
        if a.only == "identity":
            self.section("notepad", self.s_notepad)
            self.fp.write_notepad(15, self.notepad0.get(15, bytes(32)))
            return
        used = self.r.sec("state").get("used_slots", [])
        if used and not a.wipe:
            raise SystemExit(f"the module has templates in {used}: --wipe to empty it")
        try:
            self.section("notepad", self.s_notepad)
            self.section("enroll", self.s_enroll)
            self.section("templates", self.s_templates)
            self.section("match", lambda s: self.s_match(s, a.repeats))
            self.section("downchar", self.s_downchar)
            self.section("baud", self.s_baud)
            self.section("password_set", self.s_password_set)
            self.who.power_cycle("Primer apagado: ¿se conservan contraseña, libreta y plantillas?")
            self.section("after_power_cycle", self.s_after_cycle)
            self.section("templates_after_cycle", lambda s: self.s_templates(s, "after"))
            self.section("match_after_cycle", lambda s: self.s_match(s, max(3, a.repeats // 4)))
            self.section("password_tries", self.s_password_tries)
            self.section("password_clear", self.s_password_clear)
            self.section("empty_vs_notepad", self.s_empty)
            self.section("cleanup", self.s_cleanup)
            self.who.power_cycle("Segundo apagado: comprobar que el sensor queda como nuevo.")
            self.section("final", self.s_final)
        finally:
            if self.pwd_set or self.baud_changed and self.r.sec("final").get("baud") != 57600:
                self.rescue()
        self.r.d["meta"]["minutes"] = round((time.time() - t0) / 60, 1)
        self.r.save()
        self.compare_after_cycle()

    def compare_after_cycle(self):
        before, after = self.r.sec("match"), self.r.sec("match_after_cycle")
        same = [k for k in after if isinstance(after[k], dict) and "scores" in after[k]
                and before.get(k, {}).get("scores", [None])[0] == after[k]["scores"][0]]
        self.r.sec("match_after_cycle")["same_scores_as_before"] = len(same) == len(
            [k for k in after if isinstance(after[k], dict) and "scores" in after[k]])
        self.r.save()


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    g = ap.add_mutually_exclusive_group(required=True)
    g.add_argument("--emu", action="store_true", help="the bench's fp_emu (fully automatic)")
    g.add_argument("--real", action="store_true", help="a real module, touched by a person")
    ap.add_argument("--wipe", action="store_true", help="allowed to empty the module's library")
    ap.add_argument("--only", choices=["identity"], help="read-only identity check")
    ap.add_argument("--repeats", type=int, default=20, help="Match repeats per pair (20)")
    ap.add_argument("--speed", default="fast", help="fp_emu speed: fast | real")
    ap.add_argument("--leave-b-at-100", action="store_true",
                    help="end with finger B's centre template at slot 100 (to check the key denies it)")
    ap.add_argument("--out", help="report path (default r502-report-<mode>-<time>.json)")
    a = ap.parse_args()
    s = Session(a)
    try:
        s.run()
    except BaseException:
        if s.pwd_set or s.baud_changed:
            s.rescue()
        raise
    errs = s.r.d["errors"]
    print(f"\n{'DONE' if not errs else f'DONE WITH {len(errs)} ERRORS'}: {s.r.path}")
    if s.who.real:
        s.who.say("Cardea: medidas terminadas", f"{'Sin errores' if not errs else f'{len(errs)} errores'}."
                  " Ya puedes dejar el sensor.", popup=True, error=bool(errs))
    sys.exit(1 if errs else 0)


if __name__ == "__main__":
    main()
