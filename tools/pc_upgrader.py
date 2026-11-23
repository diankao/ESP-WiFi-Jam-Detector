#!/usr/bin/env python3
"""Serial firmware upgrader for the ESP32-C5-MINI-KIT-U build.

Sends a firmware image to the device over the USB Serial/JTAG link using the
ota_begin command + binary stop-and-wait frames (docs/LINK.md), lets the
device verify it (esp_ota_end checks the image SHA-256), reboot into the new
A/B slot, then reconnects to confirm the new version and (unless
--no-confirm) marks it valid so the bootloader keeps it.

    python tools/pc_upgrader.py                          # auto-detect port,
                                                         # latest build_c5_mini image
    python tools/pc_upgrader.py --port COM5 --bin build-c5-mini/app.bin
    python tools/pc_upgrader.py --corrupt-at 50          # fault injection:
                                                         # device must abort frame 50

Usage needs only pyserial:  pip install pyserial
"""

import argparse
import json
import os
import struct
import sys
import time
import zlib

try:
    import serial
    from serial.tools import list_ports
except ImportError:  # pragma: no cover
    sys.exit("pyserial is required:  pip install pyserial")

FRAME_PAYLOAD = 448          # main/ota_upgrade.h: 12-byte header + payload <= 512 B RX ring
BEGIN_TIMEOUT = 60.0         # the device erases the slot first; the CPU freezes for it
ACK_TIMEOUT = 2.0            # per-frame stop-and-wait ack
FRAME_RETRIES = 3
RECONNECT_TIMEOUT = 30.0     # waiting for the port to come back after reboot
SYNC_DRAIN = 2.0             # drop stale lines buffered while no one read the port


def auto_detect_port():
    for p in list_ports.comports():
        blob = " ".join(filter(None, (p.description, p.manufacturer, p.hwid))).lower()
        if "303a" in blob or "esp" in blob or "jtag" in blob or "usb serial" in blob:
            return p.device
    ports = list(list_ports.comports())
    return ports[0].device if ports else None


def open_port(name):
    """Open without resetting the board (docs/LINK.md): DTR low with RTS high
    is the reset combo, so keep both asserted."""
    s = serial.Serial()
    s.port = name
    s.baudrate = 115200  # ignored by USB Serial/JTAG
    s.timeout = 0.05
    s.dtr = True
    s.rts = True
    s.open()
    return s


class Dev:
    """One command in, JSON lines out; console log lines are skipped."""

    def __init__(self, port, verbose=False):
        self.ser = open_port(port)
        self.verbose = verbose
        self.cid = 0
        self.buf = b""

    def close(self):
        try:
            self.ser.close()
        except Exception:
            pass

    def send_cmd(self, cmd, **kv):
        self.cid += 1
        payload = {"c": cmd, "id": self.cid}
        payload.update(kv)
        self.ser.write((json.dumps(payload, separators=(",", ":")) + "\n").encode())

    def send_frame(self, frame):
        self.ser.write(frame)

    def _lines(self):
        self.buf += self.ser.read(4096)
        while b"\n" in self.buf:
            line, self.buf = self.buf.split(b"\n", 1)
            yield line.decode("utf-8", "replace").strip()

    def read_msg(self, timeout):
        """Next {"t":...} device message within timeout, else None."""
        end = time.monotonic() + timeout
        while time.monotonic() < end:
            for line in self._lines():
                i = line.find('{"t":')
                if i < 0:
                    if self.verbose and line:
                        print(f"  dev| {line}")
                    continue
                try:
                    return json.loads(line[i:])
                except ValueError:
                    continue
        return None

    def sync(self):
        """Drop everything stale, then prove the link with one ping round-trip."""
        deadline = time.monotonic() + SYNC_DRAIN
        while time.monotonic() < deadline:
            self.ser.read(4096)
            time.sleep(0.05)
        for _ in range(3):
            self.send_cmd("ping")
            m = self.read_msg(1.0)
            if m and m.get("t") == "ack" and m.get("id") == self.cid:
                return True
        return False

    def hello(self):
        self.send_cmd("hello")
        end = time.monotonic() + 3.0
        while time.monotonic() < end:
            m = self.read_msg(end - time.monotonic())
            if m and m.get("t") == "hello":
                return m
        return None

    def wait_ack(self, cid, timeout):
        end = time.monotonic() + timeout
        while time.monotonic() < end:
            m = self.read_msg(end - time.monotonic())
            if m and m.get("t") == "ack" and m.get("id") == cid:
                return m
        return None


def reconnect(port, timeout, verbose):
    """Wait for the port to come back after the reboot, then sync + hello."""
    end = time.monotonic() + timeout
    while time.monotonic() < end:
        time.sleep(1.0)
        if not any(p.device == port for p in list_ports.comports()):
            continue  # still gone
        try:
            dev = Dev(port, verbose)
            if dev.sync():
                h = dev.hello()
                if h:
                    return dev, h
            dev.close()
        except (OSError, serial.SerialException):
            pass
    return None, None


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--port", "-p", help="COM port (default: auto-detect)")
    ap.add_argument("--bin", "-b", metavar="FILE",
                    default=os.path.join("build-c5-mini", "esp-wifi-jammer-detector.bin"),
                    help="firmware image (default: %(default)s)")
    ap.add_argument("--corrupt-at", type=int, metavar="N", default=0,
                    help="fault injection: flip one payload byte in frame N")
    ap.add_argument("--no-reconnect", action="store_true", help="skip the post-reboot verification")
    ap.add_argument("--no-confirm", action="store_true",
                    help="do not send ota_confirm (let the 10 s self-check decide)")
    ap.add_argument("--verbose", "-v", action="store_true", help="print device console lines")
    args = ap.parse_args()

    try:
        with open(args.bin, "rb") as f:
            image = f.read()
    except OSError as e:
        sys.exit(f"cannot read {args.bin}: {e}")
    port = args.port or auto_detect_port()
    if not port:
        sys.exit("no serial port found (pass --port)")
    n_frames = (len(image) + FRAME_PAYLOAD - 1) // FRAME_PAYLOAD
    print(f"port {port}, image {len(image)} B in {n_frames} stop-and-wait frames")

    dev = Dev(port, args.verbose)
    try:
        if not dev.sync():
            sys.exit("device did not answer ping")
        h = dev.hello()
        if h:
            ota = h.get("ota", {})
            print(f"device {h.get('id')}: fw {h.get('fw')}, slot {ota.get('slot', 'factory')} "
                  f"({ota.get('img', '-')})")

        print("begin (the device erases the target slot first; this can take seconds)")
        dev.send_cmd("ota_begin", v=len(image))
        ack = dev.wait_ack(dev.cid, BEGIN_TIMEOUT)
        if not ack:
            sys.exit("no answer to ota_begin")
        if not ack.get("ok"):
            hints = {"active alarm": "wait for the band to leave ALARM, then retry",
                     "no ota": "flash the A/B partition table first (partitions_4mb.csv)",
                     "too big": "the image does not fit the ota slots; rebalance the table"}
            sys.exit(f"ota_begin refused: {ack.get('err')} -- {hints.get(ack.get('err'), '')}")

        t0 = time.monotonic()
        last_oack = None
        for seq in range(1, n_frames + 1):
            payload = image[(seq - 1) * FRAME_PAYLOAD:seq * FRAME_PAYLOAD]
            frame = struct.pack("<III", seq, len(payload), zlib.crc32(payload) & 0xFFFFFFFF) + payload
            if seq == args.corrupt_at:
                print(f"corrupt-at: flipping one payload byte in frame {seq}")
                frame = frame[:12] + bytes([frame[12] ^ 0xFF]) + frame[13:]

            ok = False
            for attempt in range(1, FRAME_RETRIES + 1):
                dev.send_frame(frame)
                m = dev.read_msg(ACK_TIMEOUT)
                if m and m.get("t") == "oack":
                    if not m.get("ok"):
                        sys.exit(f"device aborted the transfer at frame {seq}: {m.get('err')}")
                    if m.get("seq") == seq:
                        ok, last_oack = True, m
                        break
                if args.verbose:
                    print(f"  retry {attempt}: frame {seq} ({m or 'no oack'})")
            if not ok:
                sys.exit(f"frame {seq}: no valid oack after {FRAME_RETRIES} tries")

            if seq % 32 == 0 or seq == n_frames:
                done = min(seq * FRAME_PAYLOAD, len(image))
                dt = max(time.monotonic() - t0, 1e-6)
                eta = (len(image) - done) / (done / dt)
                print(f"  {done * 100 // len(image):3d}%  {done}/{len(image)} B  "
                      f"{done / dt / 1024:.0f} KB/s  eta {eta:.0f}s")

        if not last_oack.get("done"):
            sys.exit(f"transfer finished without a done oack: {last_oack}")
        print(f"image verified, slot {last_oack.get('slot')} boots next; device reboots in ~5 s")

        if args.no_reconnect:
            return
        dev.close()
        print("waiting for the device to come back ...")
        dev, h = reconnect(port, RECONNECT_TIMEOUT, args.verbose)
        if not dev:
            sys.exit("the port never came back (power-cycle the board; the old slot still boots)")
        ota = h.get("ota", {})
        print(f"back up: fw {h.get('fw')}, slot {ota.get('slot')}, image {ota.get('img')}")
        if ota.get("img") == "pending":
            if args.no_confirm:
                print("left pending: the 10 s self-check will confirm it (or roll back)")
            else:
                dev.send_cmd("ota_confirm")
                m = dev.wait_ack(dev.cid, 3.0)
                print("ota_confirm:", "ok" if m and m.get("ok") else m)
        print("upgrade complete")
    finally:
        dev.close()


if __name__ == "__main__":
    main()
