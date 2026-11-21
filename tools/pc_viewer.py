#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""
Real-time RF Spectrum & Waterfall Viewer for ESP-WiFi-Jam-Detector
(Waveshare ESP32-C5-MINI-KIT-U & ESP32-C5-Touch-LCD-3.5).

Connects over USB Serial/JTAG using the JSON Lines link protocol (link.c):
  - Displays live 2.4 GHz / 5 GHz spectrum (avg, max, learned baseline)
  - Displays scrolling waterfall spectrogram
  - Shows per-band detector state (LEARNING / NORMAL / SUSPECT / ALARM)
  - Provides one-click [Re-learn Baseline], [Auto-range Gain], and [Beep] controls

Usage:
    python tools/pc_viewer.py --port COM5
    python tools/pc_viewer.py                  # auto-detect ESP32-C5 USB port
"""

import argparse
import json
import sys
import threading
import time
from collections import deque

try:
    import numpy as np
    import serial
    import serial.tools.list_ports
    import matplotlib.pyplot as plt
    from matplotlib.animation import FuncAnimation
    from matplotlib.widgets import Button
except ImportError as e:
    print(f"Missing dependency: {e}")
    print("Install with: pip install pyserial numpy matplotlib")
    sys.exit(1)


WATERFALL_ROWS = 120
SPEC_POINTS = 100

STATE_COLORS = {
    "learning": "#3b82f6",
    "normal": "#22c55e",
    "suspect": "#f59e0b",
    "alarm": "#ef4444",
}


def auto_detect_port() -> str | None:
    ports = list(serial.tools.list_ports.comports())
    for p in ports:
        desc = f"{p.description} {p.manufacturer or ''} {p.hwid}".lower()
        if "303a" in desc or "esp" in desc or "jtag" in desc or "usb serial" in desc:
            return p.device
    return ports[0].device if ports else None


class LinkClient:
    def __init__(self, port: str, baud: int = 115200):
        self.port = port
        self.baud = baud
        self.ser = serial.Serial(port, baud, timeout=0.1)
        self.lock = threading.Lock()
        self.running = True
        self.cmd_id = 1

        self.active_band = "2.4"
        self.hello: dict = {}
        self.status: dict = {}
        self.spec: dict[str, dict] = {}
        self.waterfall: dict[str, np.ndarray] = {
            "2.4": np.full((WATERFALL_ROWS, SPEC_POINTS), np.nan, dtype=np.float32),
            "5": np.full((WATERFALL_ROWS, SPEC_POINTS), np.nan, dtype=np.float32),
        }
        self.events: deque[str] = deque(maxlen=4)
        self.last_ack: str = ""

        self._rx_thread = threading.Thread(target=self._rx_loop, daemon=True)
        self._tx_thread = threading.Thread(target=self._poll_loop, daemon=True)
        self._rx_thread.start()
        self._tx_thread.start()

    def send_cmd(self, cmd: str, **kwargs) -> int:
        with self.lock:
            cid = self.cmd_id
            self.cmd_id += 1
        payload = {"c": cmd, "id": cid}
        payload.update(kwargs)
        line = (json.dumps(payload, separators=(",", ":")) + "\n").encode("utf-8")
        try:
            self.ser.write(line)
        except Exception as e:
            with self.lock:
                self.last_ack = f"TX error: {e}"
        return cid

    def _poll_loop(self):
        time.sleep(0.2)
        self.send_cmd("hello")
        last_status_t = 0.0
        while self.running:
            now = time.monotonic()
            with self.lock:
                band = self.active_band
            self.send_cmd("spec", k=band)
            if now - last_status_t >= 2.0:
                self.send_cmd("get")
                last_status_t = now
            time.sleep(0.25)

    def _rx_loop(self):
        buf = b""
        while self.running:
            try:
                chunk = self.ser.read(1024)
            except Exception:
                time.sleep(0.1)
                continue
            if not chunk:
                continue
            buf += chunk
            while b"\n" in buf:
                raw, buf = buf.split(b"\n", 1)
                line = raw.decode("utf-8", errors="replace").strip()
                if not line.startswith('{"t":'):
                    continue
                try:
                    msg = json.loads(line)
                except json.JSONDecodeError:
                    continue
                self._handle_msg(msg)

    def _handle_msg(self, msg: dict):
        t = msg.get("t")
        with self.lock:
            if t == "hello":
                self.hello = msg
            elif t == "status":
                self.status = msg
            elif t == "ack":
                ok = msg.get("ok", False)
                err = msg.get("err", "")
                self.last_ack = "OK" if ok else f"ERR: {err}"
            elif t == "event":
                ev_str = (
                    f"[{msg.get('band')}G] {msg.get('from')} -> {msg.get('to')} "
                    f"({msg.get('kind')}, floor {msg.get('floor_db', 0):+.1f} dB)"
                )
                self.events.append(ev_str)
            elif t == "spec":
                band = msg.get("band", "2.4")
                avg = np.array(msg.get("avg", []), dtype=np.float32) / 10.0
                mx = np.array(msg.get("max", []), dtype=np.float32) / 10.0
                base = msg.get("base")
                base_arr = (
                    np.array(base, dtype=np.float32) / 10.0 if base is not None else None
                )
                if avg.size == SPEC_POINTS:
                    self.spec[band] = {
                        "f0": msg.get("f0_khz", 2400000) / 1000.0,
                        "f1": msg.get("f1_khz", 2484000) / 1000.0,
                        "gain": msg.get("gain", 0),
                        "sweeps": msg.get("sweeps", 0),
                        "avg": avg,
                        "max": mx,
                        "base": base_arr,
                    }
                    wf = self.waterfall[band]
                    wf[1:] = wf[:-1]
                    wf[0] = avg

    def close(self):
        self.running = False
        try:
            self.ser.close()
        except Exception:
            pass


def main():
    parser = argparse.ArgumentParser(description="ESP-WiFi-Jam-Detector PC Spectrum Viewer")
    parser.add_argument("--port", "-p", help="Serial port (e.g. COM5, /dev/ttyACM0)")
    parser.add_argument("--baud", "-b", type=int, default=115200, help="Baud rate")
    args = parser.parse_args()

    port = args.port or auto_detect_port()
    if not port:
        print("No serial port found. Connect the ESP32-C5 board or pass --port COMx.")
        sys.exit(1)

    print(f"Connecting to {port} ...")
    client = LinkClient(port, args.baud)

    plt.style.use("dark_background")
    fig = plt.figure(figsize=(12, 7.6), facecolor="#0f172a")
    fig.canvas.manager.set_window_title(f"ESP-WiFi-Jam-Detector ({port})")

    gs = fig.add_gridspec(
        2, 1, height_ratios=[1.15, 1.0], left=0.07, right=0.97, top=0.83, bottom=0.12, hspace=0.22
    )
    ax_spec = fig.add_subplot(gs[0], facecolor="#111827")
    ax_wf = fig.add_subplot(gs[1], facecolor="#111827")

    (line_avg,) = ax_spec.plot([], [], color="#38bdf8", lw=1.6, label="Avg (dB)")
    (line_max,) = ax_spec.plot([], [], color="#f59e0b", lw=1.0, alpha=0.85, label="Max (dB)")
    (line_base,) = ax_spec.plot([], [], color="#22c55e", lw=1.2, ls="--", alpha=0.9, label="Baseline")
    ax_spec.set_ylabel("Rel. Power (dB)")
    ax_spec.grid(True, alpha=0.2, color="#94a3b8")
    ax_spec.legend(loc="upper right", frameon=True, facecolor="#1e293b", edgecolor="#334155")

    wf_img = ax_wf.imshow(
        np.full((WATERFALL_ROWS, SPEC_POINTS), 20.0, dtype=np.float32),
        aspect="auto",
        origin="upper",
        cmap="inferno",
        interpolation="nearest",
        vmin=15.0,
        vmax=65.0,
    )
    ax_wf.set_xlabel("Frequency (MHz)")
    ax_wf.set_ylabel("Time (newest top)")

    # Header status text
    title_text = fig.text(
        0.07, 0.945, f"ESP-WiFi-Jam-Detector  |  Port: {port}  |  Connecting...",
        fontsize=12, fontweight="bold", color="#f8fafc",
    )
    status_24_text = fig.text(0.07, 0.895, "2.4 GHz: --", fontsize=10, color="#cbd5e1")
    status_5_text = fig.text(0.50, 0.895, "5 GHz: --", fontsize=10, color="#cbd5e1")
    event_text = fig.text(0.07, 0.855, "Events: (none)", fontsize=9, color="#94a3b8")

    # Control buttons along the bottom
    btn_specs = [
        (0.07, 0.025, 0.11, 0.05, "2.4 GHz"),
        (0.19, 0.025, 0.11, 0.05, "5 GHz"),
        (0.34, 0.025, 0.16, 0.05, "Re-learn Baseline"),
        (0.52, 0.025, 0.16, 0.05, "Auto-range Gain"),
        (0.70, 0.025, 0.12, 0.05, "Toggle Beep"),
        (0.84, 0.025, 0.13, 0.05, "Clear Waterfall"),
    ]
    buttons = []
    for x, y, w, h, label in btn_specs:
        bax = fig.add_axes([x, y, w, h])
        btn = Button(bax, label, color="#1e293b", hovercolor="#334155")
        btn.label.set_color("#f8fafc")
        btn.label.set_fontsize(9)
        buttons.append(btn)

    def on_band_24(_):
        with client.lock:
            client.active_band = "2.4"

    def on_band_5(_):
        with client.lock:
            client.active_band = "5"

    def on_relearn(_):
        client.send_cmd("relearn")

    def on_autorange(_):
        client.send_cmd("autorange")

    def on_beep(_):
        with client.lock:
            cur = client.status.get("set", {}).get("beep", True)
        client.send_cmd("set", k="beep", v=0 if cur else 1)

    def on_clear_wf(_):
        with client.lock:
            client.waterfall[client.active_band][:] = np.nan

    buttons[0].on_clicked(on_band_24)
    buttons[1].on_clicked(on_band_5)
    buttons[2].on_clicked(on_relearn)
    buttons[3].on_clicked(on_autorange)
    buttons[4].on_clicked(on_beep)
    buttons[5].on_clicked(on_clear_wf)

    def update(_frame):
        with client.lock:
            band = client.active_band
            hello = dict(client.hello)
            status = dict(client.status)
            spec = client.spec.get(band)
            wf = client.waterfall[band].copy()
            events = list(client.events)
            last_ack = client.last_ack

        model = hello.get("model", "ESP32-C5")
        fw = hello.get("fw", "?")
        up = status.get("up", 0)
        beep = status.get("set", {}).get("beep", True)
        title_text.set_text(
            f"{model} (v{fw})  |  {port}  |  View: {band} GHz  |  "
            f"Uptime: {up}s  |  Beep: {'ON' if beep else 'OFF'}"
            + (f"  |  Cmd: {last_ack}" if last_ack else "")
        )

        bands_info = status.get("bands", {})
        for bkey, txt_obj in (("2.4", status_24_text), ("5", status_5_text)):
            bi = bands_info.get(bkey)
            if bi:
                st = bi.get("state", "?")
                color = STATE_COLORS.get(st, "#cbd5e1")
                extra = f"{bi.get('learn_pct', 0)}%" if st == "learning" else bi.get("kind", "none")
                txt_obj.set_text(
                    f"{bkey} GHz: [{st.upper()} {extra}]  "
                    f"floor {bi.get('floor_db', 0):+.1f} dB  "
                    f"occ {bi.get('occupancy_pct', 0)}%  "
                    f"clip {bi.get('clipped_pct', 0)}%  "
                    f"alarms {bi.get('alarms', 0)}"
                )
                txt_obj.set_color(color)

        if events:
            event_text.set_text("Latest event: " + events[-1])

        if spec:
            freqs = np.linspace(spec["f0"], spec["f1"], SPEC_POINTS)
            line_avg.set_data(freqs, spec["avg"])
            line_max.set_data(freqs, spec["max"])
            if spec["base"] is not None:
                line_base.set_data(freqs, spec["base"])
            else:
                line_base.set_data([], [])

            ax_spec.set_xlim(spec["f0"], spec["f1"])
            valid_vals = np.concatenate([spec["avg"], spec["max"]])
            lo = float(np.nanmin(valid_vals)) - 5.0
            hi = float(np.nanmax(valid_vals)) + 8.0
            if hi - lo < 25.0:
                hi = lo + 25.0
            ax_spec.set_ylim(lo, hi)
            ax_spec.set_title(
                f"{band} GHz Live Spectrum  (gain={spec['gain']}, sweeps={spec['sweeps']})",
                fontsize=10,
                color="#e2e8f0",
            )

            finite = wf[np.isfinite(wf)]
            if finite.size:
                vmin = float(np.percentile(finite, 5))
                vmax = float(np.percentile(finite, 99))
                if vmax - vmin < 15.0:
                    vmax = vmin + 15.0
                wf_img.set_clim(vmin, vmax)
            wf_img.set_data(wf)
            wf_img.set_extent([spec["f0"], spec["f1"], WATERFALL_ROWS, 0])

        return line_avg, line_max, line_base, wf_img

    _anim = FuncAnimation(fig, update, interval=200, blit=False, cache_frame_data=False)
    try:
        plt.show()
    finally:
        client.close()


if __name__ == "__main__":
    main()
