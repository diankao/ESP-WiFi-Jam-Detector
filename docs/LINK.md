# USB link protocol (version 1)

The detector talks to a host (the Home Assistant bridge, or anything else) over
the board's USB-C port: the ESP32-C5's built-in USB Serial/JTAG, which shows up
as `/dev/ttyACM*` (use `/dev/serial/by-id/usb-Espressif_USB_JTAG_serial_debug_unit_<MAC>-if00`).
The same port carries the console log. Firmware: `main/link_proto.c` (format),
`main/link.c` (tasks), `main/params.c` (settings table).

## Opening the port without resetting the board

The USB Serial/JTAG resets the chip when the host sets **DTR low with RTS
high** (how `idf.py` / esptool reset it). Linux raises both lines when a port
is opened; pyserial's default then lowers DTR before RTS, which resets the
board. Keep both high:

```python
s = serial.Serial()
s.port = "/dev/ttyACM0"
s.dtr = True
s.rts = True
s.open()
```

Checked on the board: reopening this way 3 times kept the uptime counting
(no `rst:` line). Closing the port does not reset it either. Baud rate is ignored.

## Framing

- One JSON object per line, UTF-8, ending `\r\n` (the console adds the `\r`).
- Device messages start with `{"t":`. Every other line is console log
  (`I (1234) tag: ...`) and should be ignored. To be robust against a log line
  cut short by a reset, look for `{"t":` anywhere in the line and parse from
  there; drop lines that do not parse.
- Every device message has `"v"` (protocol version). A host should ignore
  fields it does not know and warn if `v` is higher than it supports.
- **Streaming needs a host.** The device sends `status` and `event` only
  while a host has sent a command in the last 60 s. Send `ping` every 15 s or
  so. With no host, nothing is written (so nothing piles up and no USB traffic
  is made).
- **Old lines come first.** The USB driver keeps up to 2 KB that was written
  while no program read the port, and it keeps the *oldest* lines. They arrive
  first after the port is opened and can be minutes old. A host should ignore
  everything until the answer to its first command: send
  `{"c":"ping","id":<random>}` and drop all messages until the `ack` with that
  id. The bridge does this.
- If the host stops reading, the device drops messages; it never waits.

## Device → host

### `hello`

Sent in answer to the `hello` command (then a `status`). `id` is
the chip's base MAC (stable, unique). `params` describes every adjustable
setting, so a host can build its controls from it. `ota` is present only on a
build with A/B slots (the C5 mini kit): the running slot and its state.

```json
{"t":"hello","v":1,"id":"10bda3fefff2","fw":"1.1.0","model":"ESP32-C5-MINI-KIT-U","bands":["2.4","5"],
 "params":[{"k":"learn_s","n":"Learn time","u":"s","min":10,"max":300,"step":10},
           {"k":"wide_db","n":"Wideband rise","u":"dB","min":3,"max":30,"step":1}, ...],
 "ota":{"slot":"ota_0","img":"valid"}}
```

`img` is one of `new`, `valid`, `invalid`, `aborted` or `pending`
(`pending` = just upgraded, waiting for the app to confirm itself).

### `status`

While a host is active: every 10 s, after every state change and after every
command (at most two per second).

```json
{"t":"status","v":1,"up":3600,"boot":29,
 "bands":{"2.4":{"scan":true,"state":"alarm","kind":"pulsed","learn_pct":100,"alarms":3,
                 "last_alarm_kind":"pulsed","floor_db":12.4,"occupancy_pct":41,"clipped_pct":0,"duty_pct":90},
          "5":{"scan":false,"state":"normal","kind":"none", ...}},
 "power":{"vbus":true,"battery":true,"charging":false,"pct":85,"mv":4012},
 "set":{"beep":false,"scan":1,"learn_s":60,"wide_db":10,"wide_pct":60,"cw_db":15,"cw_pct":90,"swept_db":15,
        "swept_pct":60,"duty_pct":80,"hold_s":5,"clear_s":10,"weight":6,"brightness":70}}
```

| Field | Meaning |
|---|---|
| `up` | seconds since boot |
| `boot` | boot counter (matches the TF-card log's `boot` column) |
| `bands.<b>.scan` | band is being swept (a band that is off never alarms) |
| `state` | `learning`, `normal`, `suspect`, `alarm` |
| `kind` | reason for `suspect` / `alarm`: `wideband`, `swept`, `narrowband`, `pulsed`; `none` otherwise |
| `learn_pct` | baseline learning progress |
| `alarms`, `last_alarm_kind` | alarms since boot and the kind of the latest |
| `floor_db` | median noise-floor rise over the learned baseline, dB |
| `occupancy_pct`, `clipped_pct`, `duty_pct` | latest sweep's metrics (as on the status screen) |
| `power` | absent if the power chip could not be read; `pct`/`mv` absent without a battery |
| `set` | current settings: `beep`, `scan` (bit 0 = 2.4 GHz, bit 1 = 5 GHz) and every `params` key |

### `event`

One per detector state change while a host is active, with the same fields as a row of the TF-card
CSV log (`up` is uptime in seconds, other names as the CSV header).

```json
{"t":"event","v":1,"boot":29,"up":19.7,"band":"2.4","from":"normal","to":"suspect","kind":"swept",
 "floor_db":-0.28,"continuous_db":-0.22,"max_cell_db":0.05,"max_cell_mhz":2455.0, ...}
```

### `ack`

Answer to every command except `hello`. `id` echoes the command's `id` (0 if
it had none, or if the line could not be parsed).

```json
{"t":"ack","v":1,"id":11,"ok":true}
{"t":"ack","v":1,"id":12,"ok":false,"err":"bad key or value"}
```

Errors: `parse`, `too long` (over 255 bytes), `unknown command`,
`missing k or v`, `bad key or value`, `busy`, `failed`, `missing v`, `no ota`.

### `oack`

One per OTA frame during a firmware upgrade (see below). `id` echoes the
`ota_begin` command's id.

```json
{"t":"oack","v":1,"id":11,"ok":true,"seq":42,"bytes":18816}
{"t":"oack","v":1,"id":11,"ok":true,"seq":1856,"bytes":831200,"done":true,"slot":"ota_1"}
{"t":"oack","v":1,"id":11,"ok":false,"err":"crc"}
```

`done` appears on the last frame: the image passed the SHA-256 check and the
boot slot was switched. Error strings: `seq`, `crc`, `size`, `flash`,
`verify`, `boot`, `timeout`, `abort` -- any of them ends the transfer.

## Host → device

Flat JSON objects, one per line (`\n` or `\r\n`). String, integer and
`true`/`false` values only; no escapes, nesting or floats. `id` is optional
(integers up to 4000000). Any valid command makes the host active for 60 s.

| Command | Effect |
|---|---|
| `{"c":"ping","id":1}` | `ack` only; keeps the host active |
| `{"c":"hello"}` | answers with `hello` (then a `status`) |
| `{"c":"get","id":1}` | `ack`, then a `status` |
| `{"c":"relearn","id":2}` | forgets both baselines and learns again (as a long BOOT press) |
| `{"c":"set","k":"beep","v":true,"id":3}` | alarm beep on/off |
| `{"c":"set","k":"scan","v":3,"id":4}` | bands scanned: 1 = 2.4 GHz, 2 = 5 GHz, 3 = both |
| `{"c":"set","k":"wide_db","v":8,"id":5}` | any `params` key; the value must be within `min`..`max` and on a `step` |
| `{"c":"ota_begin","v":831200,"id":6}` | start a firmware upgrade (see below) |
| `{"c":"ota_abort","id":7}` | cancel an upgrade, resume the sweep |
| `{"c":"ota_reboot","id":8}` | reboot now (after `ota_begin`...`done`) |
| `{"c":"ota_confirm","id":9}` | mark the running image valid (cancel the rollback) |

A `set` works exactly like moving the control on the device: it is applied at
once, saved to flash, and the settings screen shows the new value. Edits still
pending on the screen are saved first, so neither side's change is lost.

## Serial OTA upgrade

`tools/pc_upgrader.py` implements the host side; the device side is
`main/ota_upgrade.c` (engine) + the link commands above. On a build with the
A/B partition table (`partitions_4mb.csv`): `ota_0`/`ota_1` 1280 KB each,
`otadata` 8 KB, and `CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE=y`.

1. `ota_begin` with `v` = exact image size. The device refuses while a band
   is in `alarm` (`active alarm`), pauses the sweep, and **erases the target
   slot -- several seconds during which the CPU is frozen**; the `ack` comes
   only after the erase, so allow ~60 s for it. Acks: `busy`, `active alarm`,
   `no ota` (factory-only build), `too big`, `flash`.
2. After the `ok` ack the link carries **binary frames** instead of JSON
   lines: `[u32 seq][u32 len][u32 crc32]` little-endian, then `len` (1..448)
   payload bytes. 460 B max fits the link's 512 B RX ring. seq starts at 1;
   the host sends frame N+1 only after the `oack` for N (stop-and-wait: flash
   writes freeze the single CPU core, so pipelining would drop bytes). The
   CRC is plain CRC-32/ISO-HDLC (`zlib.crc32`).
3. Every frame gets one `oack` (`seq`, `bytes`). Any error (`seq`, `crc`,
   `size`, `flash`) or 5 s of silence aborts the transfer and resumes the
   sweep; the host restarts from frame 1 (no resume).
4. When `bytes` reaches the image size the device runs `esp_ota_end`
   (verifies the image SHA-256), switches `otadata` to the new slot and
   answers `done:true` with the slot name; ~5 s later it reboots. A power cut
   anywhere before that simply keeps the current app running.

### Rollback

The upgraded app boots as `pending` (`ESP_OTA_IMG_PENDING_VERIFY`). It
confirms itself after a 10 s self-check (sweep alive), or immediately on the
`ota_confirm` command. If it crashes, hangs or never confirms, the next reset
makes the bootloader boot the previous slot instead -- a broken image cannot
brick the device. `hello`'s `ota.img` shows the state.

### Extension points (not enabled)

- **Signed images**: Secure Boot V2 (RSA/ECDSA) makes the bootloader reject
  images not signed with the project key; combine with flash encryption for
  confidentiality. Off here: hobby device, and enabling burns eFuses.
- **Anti-rollback**: `CONFIG_BOOTLOADER_APP_ANTI_ROLLBACK` + a monotonic
  `security_version` in eFuse refuses to boot images older than the running
  one. Off for the same reason -- **eFuse changes are permanent**.

## Radio side

Device messages go out inside the sweep's radio-quiet window, like LCD and
TF-card transfers, so USB traffic does not overlap a capture. Console log
lines are not gated; the periodic per-sweep detector lines are now at debug
level, so the default log is mostly state changes and the 10 s sweep summary.
