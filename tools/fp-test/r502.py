"""GROW R502 family fingerprint module: protocol driver for tests, over the Pico test host.

Reference implementation of the host side, written from the R502-F Pro user manual (V1.1,
2024-03). It talks to the module through tools/pico-fp-host (a Pico 2 that exposes UART1, WAKEUP
and the power switch on USB serial), so the same tests run against the bench's emulator (fp_emu)
and against a real module wired to the Pico.

    link = HostLink()            # Pico running fp_host (2e8a:0009 / 000a)
    fp = R502(link)
    link.power(True); fp.wait_ready()
    fp.handshake()

Bench (ESP32-S3 console, for the emulator's "fp ..." commands): Bench().fp("finger A").

SPDX-License-Identifier: AGPL-3.0-or-later
"""
import os
import queue
import threading
import time

import serial
import serial.tools.list_ports

HOST_IDS = {(0x2E8A, 0x0009), (0x2E8A, 0x000A)}   # pico-sdk stdio USB (RP2350 / RP2040)
BENCH_IDS = {(0x1A86, 0x55D3), (0x303A, 0x1001)}

PID_CMD, PID_DATA, PID_ACK, PID_END = 0x01, 0x02, 0x07, 0x08

CODES = {
    0x00: "ok", 0x01: "packet error", 0x02: "no finger", 0x03: "image capture failed",
    0x06: "image too messy", 0x07: "too few features", 0x08: "no match", 0x09: "not found",
    0x0A: "merge failed", 0x0B: "id out of range", 0x0C: "template read error / invalid",
    0x0D: "upload failed", 0x0E: "cannot receive data", 0x0F: "cannot send data",
    0x10: "delete failed", 0x11: "empty failed", 0x13: "wrong password", 0x15: "no valid image",
    0x18: "flash error", 0x1A: "invalid register", 0x1B: "invalid data", 0x1F: "library full",
    0x20: "address error", 0x21: "password required", 0x22: "template empty / slot used",
    0x24: "library empty", 0x26: "timeout", 0x27: "fingerprint already enrolled",
    0x29: "sensor hardware error", 0xFC: "unsupported command", 0xFD: "hardware error",
    0xFE: "command failed",
}


class Timeout(Exception):
    pass


class BadChecksum(Exception):
    pass


def find_port(ids, env):
    if os.environ.get(env):
        return os.environ[env]
    for p in serial.tools.list_ports.comports():
        if (p.vid, p.pid) in ids:
            return p.device
    raise SystemExit(f"no serial port for {sorted(ids)} (or set {env})")


class HostLink:
    """The Pico test host (tools/pico-fp-host)."""

    def __init__(self, port=None):
        self.s = serial.Serial(port or find_port(HOST_IDS, "FP_HOST_PORT"), 115200, timeout=0.05)
        self.rx = bytearray()
        self.rx_lock = threading.Condition()
        self.replies = queue.Queue()
        self.wake_events = queue.Queue()
        self.alive = True
        self.t = threading.Thread(target=self._reader, daemon=True)
        self.t.start()
        self.cmd("ping")

    def _reader(self):
        buf = b""
        while self.alive:
            buf += self.s.read(4096)
            while b"\n" in buf:
                line, buf = buf.split(b"\n", 1)
                line = line.decode(errors="replace").strip()
                if line.startswith("RX "):
                    with self.rx_lock:
                        self.rx += bytes.fromhex(line[3:])
                        self.rx_lock.notify_all()
                elif line.startswith("WAKE "):
                    self.wake_events.put(int(line[5:]))
                elif line.startswith(("OK", "ERR")):
                    self.replies.put(line)

    def cmd(self, line, timeout=3.0):
        self.s.write((line + "\n").encode())
        try:
            r = self.replies.get(timeout=timeout)
        except queue.Empty:
            raise Timeout(f"fp_host: no answer to {line!r}")
        if r.startswith("ERR"):
            raise RuntimeError(f"fp_host: {r}")
        return r

    def tx(self, data):
        for i in range(0, len(data), 400):
            self.cmd("tx " + data[i:i + 400].hex())

    def read(self, n, timeout):
        end = time.time() + timeout
        with self.rx_lock:
            while len(self.rx) < n:
                left = end - time.time()
                if left <= 0:
                    raise Timeout(f"{len(self.rx)} of {n} bytes")
                self.rx_lock.wait(left)
            out = bytes(self.rx[:n])
            del self.rx[:n]
            return out

    def pending(self):
        with self.rx_lock:
            return bytes(self.rx)

    def flush(self):
        with self.rx_lock:
            self.rx.clear()

    def power(self, on):
        self.cmd(f"pwr {1 if on else 0}")

    def wake(self):
        return int(self.cmd("wake").split()[-1])

    def rxpin(self):
        return int(self.cmd("rxpin").split()[-1])

    def baud(self, b):
        self.cmd(f"baud {b}")

    def close(self):
        self.alive = False
        self.t.join(1)
        self.s.close()


class Bench:
    """The bench ESP32-S3's console (fp_emu commands). DTR and RTS asserted: no auto-reset."""

    def __init__(self, port=None):
        self.s = serial.Serial()
        self.s.port = port or find_port(BENCH_IDS, "CARDEA_BENCH_PORT")
        self.s.baudrate = 115200
        self.s.timeout = 0.1
        self.s.dtr = True
        self.s.rts = True
        self.s.open()
        self.s.write(b"\n")
        time.sleep(0.1)
        self.s.reset_input_buffer()

    def cmd(self, line, timeout=5.0):
        self.s.write((line + "\n").encode())
        end = time.time() + timeout
        buf = b""
        while time.time() < end:
            buf += self.s.read(256)
            while b"\n" in buf:
                ln, buf = buf.split(b"\n", 1)
                ln = ln.decode(errors="replace").strip()
                if ln.startswith("BENCH "):
                    if ln.startswith("BENCH ERR"):
                        raise RuntimeError(ln)
                    return ln[6:]
        raise Timeout(f"bench: no answer to {line!r}")

    def fp(self, line):
        return self.cmd("fp " + line)


def checksum(b):
    return sum(b) & 0xFFFF


class R502:
    def __init__(self, link, addr=0xFFFFFFFF, pkt=128):
        self.link = link
        self.addr = addr
        self.pkt = pkt

    # -- packets
    def packet(self, pid, payload, addr=None):
        a = self.addr if addr is None else addr
        body = bytes([pid]) + (len(payload) + 2).to_bytes(2, "big") + payload
        return b"\xEF\x01" + a.to_bytes(4, "big") + body + checksum(body).to_bytes(2, "big")

    def send(self, pid, payload, addr=None):
        self.link.tx(self.packet(pid, payload, addr))

    def recv(self, timeout=2.0):
        """Next packet -> (pid, payload). Skips bytes before a header (0x55, noise)."""
        end = time.time() + timeout
        while True:
            b = self.link.read(1, max(0.0, end - time.time()))
            if b != b"\xEF":
                continue
            if self.link.read(1, max(0.0, end - time.time())) != b"\x01":
                continue
            hdr = self.link.read(7, max(0.0, end - time.time()))
            n = int.from_bytes(hdr[5:7], "big")
            body = self.link.read(n, max(0.0, end - time.time()) + n / 5000)
            if checksum(hdr[4:7] + body[:-2]) != int.from_bytes(body[-2:], "big"):
                raise BadChecksum(hdr.hex() + body.hex())
            return hdr[4], body[:-2]

    def command(self, code, params=b"", timeout=2.0, addr=None):
        """Sends a command, returns (confirmation code, rest of the answer)."""
        self.send(PID_CMD, bytes([code]) + params, addr)
        pid, p = self.recv(timeout)
        if pid != PID_ACK:
            raise RuntimeError(f"expected an answer packet, got pid 0x{pid:02x}")
        return p[0], p[1:]

    def recv_data(self, timeout=10.0):
        out, n = b"", 0
        while True:
            pid, p = self.recv(timeout)
            if pid not in (PID_DATA, PID_END):
                raise RuntimeError(f"expected a data packet, got pid 0x{pid:02x}")
            out += p
            n += 1
            if pid == PID_END:
                return out, n

    def send_data(self, data):
        for i in range(0, len(data), self.pkt):
            self.send(PID_END if i + self.pkt >= len(data) else PID_DATA, data[i:i + self.pkt])

    def wait_ready(self, timeout=1.0):
        """The 0x55 byte the module sends once it has booted."""
        if self.link.read(1, timeout) != b"\x55":
            raise RuntimeError("first byte after power-on is not 0x55")

    # -- commands
    def get_image(self): return self.command(0x01)[0]
    def gen_char(self, buf): return self.command(0x02, bytes([buf]), timeout=3)[0]

    def match(self):
        c, r = self.command(0x03)
        return c, int.from_bytes(r[:2], "big")

    def search(self, buf=1, start=0, num=1500):
        c, r = self.command(0x04, bytes([buf]) + start.to_bytes(2, "big") + num.to_bytes(2, "big"))
        return c, int.from_bytes(r[:2], "big"), int.from_bytes(r[2:4], "big")

    def reg_model(self): return self.command(0x05)[0]
    def store(self, buf, slot): return self.command(0x06, bytes([buf]) + slot.to_bytes(2, "big"))[0]
    def load(self, buf, slot): return self.command(0x07, bytes([buf]) + slot.to_bytes(2, "big"))[0]

    def up_char(self, buf):
        c, _ = self.command(0x08, bytes([buf]))
        return (c, *self.recv_data()) if c == 0 else (c, b"", 0)

    def down_char(self, buf, data):
        c, _ = self.command(0x09, bytes([buf]))
        if c == 0:
            self.send_data(data)
        return c

    def up_image(self):
        c, _ = self.command(0x0A)
        return (c, *self.recv_data(timeout=20)) if c == 0 else (c, b"", 0)

    def down_image(self, data):
        c, _ = self.command(0x0B)
        if c == 0:
            self.send_data(data)
        return c

    def delete(self, start, num=1):
        return self.command(0x0C, start.to_bytes(2, "big") + num.to_bytes(2, "big"))[0]

    def empty(self): return self.command(0x0D)[0]
    def set_sys_para(self, idx, val): return self.command(0x0E, bytes([idx, val]))[0]

    def read_sys_para(self):
        c, r = self.command(0x0F)
        g = lambda o, n=2: int.from_bytes(r[o:o + n], "big")
        return c, dict(status=g(0), sysid=g(2), capacity=g(4), security=g(6), addr=g(8, 4),
                       pkt_code=g(12), baud_n=g(14))

    def set_pwd(self, pwd): return self.command(0x12, pwd.to_bytes(4, "big"))[0]
    def vfy_pwd(self, pwd): return self.command(0x13, pwd.to_bytes(4, "big"))[0]

    def random(self):
        c, r = self.command(0x14)
        return c, int.from_bytes(r[:4], "big")

    def set_addr(self, addr): return self.command(0x15, addr.to_bytes(4, "big"))[0]

    def read_inf_page(self):
        c, _ = self.command(0x16)
        return (c, *self.recv_data()) if c == 0 else (c, b"", 0)

    def write_notepad(self, page, data32): return self.command(0x18, bytes([page]) + data32)[0]

    def read_notepad(self, page):
        c, r = self.command(0x19, bytes([page]))
        return c, r[:32]

    def template_num(self):
        c, r = self.command(0x1D)
        return c, int.from_bytes(r[:2], "big")

    def index_table(self, page=0):
        c, r = self.command(0x1F, bytes([page]))
        return c, {page * 256 + k for k in range(256) if r[k // 8] >> (k % 8) & 1}

    def cancel(self): return self.command(0x30)[0]
    def handshake(self, timeout=2.0): return self.command(0x40, timeout=timeout)[0]

    def alg_ver(self):
        c, r = self.command(0x39)
        return c, r.rstrip(b"\0").decode(errors="replace")

    def fw_ver(self):
        c, r = self.command(0x3A)
        return c, r.rstrip(b"\0").decode(errors="replace")

    def aura(self, ctrl, speed, color, times): return self.command(0x35, bytes([ctrl, speed, color, times]))[0]

    # -- automatic operations: send, then read the step packets
    def auto_enroll_start(self, slot=0x05DC, overwrite=False, dup_ok=False, steps=True, lift=True):
        self.send(PID_CMD, bytes([0x31]) + slot.to_bytes(2, "big") +
                  bytes([overwrite, dup_ok, steps, lift]))

    def auto_identify_start(self, grade=3, start=0, num=1500, steps=True, max_err=1):
        self.send(PID_CMD, bytes([0x32, grade]) + start.to_bytes(2, "big") + num.to_bytes(2, "big") +
                  bytes([steps, max_err]))

    def auto_packet(self, timeout=20.0):
        """One AutoEnroll (code, step, id) or AutoIdentify (code, step, id, score) packet."""
        pid, p = self.recv(timeout)
        if pid != PID_ACK:
            raise RuntimeError(f"expected an answer packet, got pid 0x{pid:02x}")
        if len(p) == 4:
            return p[0], p[1], int.from_bytes(p[2:4], "big")
        return p[0], p[1], int.from_bytes(p[2:4], "big"), int.from_bytes(p[4:6], "big")
