/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Serial firmware upgrade over the USB link (host tool: tools/pc_upgrader.py).
 *
 * Flow: the host sends {"c":"ota_begin","v":<image size in bytes>}; after the
 * gate below passes and the target slot is erased, every received byte is a
 * binary frame until the whole image is in, verified (esp_ota_end checks the
 * image SHA-256) and the boot slot switched. An app booted this way comes up
 * PENDING_VERIFY and must confirm itself (boot self-check or ota_confirm);
 * otherwise the bootloader rolls back to the previous slot on the next reset.
 *
 * Unhappy paths are the point of the design: a dropped USB cable, a corrupt
 * frame, a power cut mid-write and a broken image all end in either "old app
 * keeps running" or "old app boots on the next reset" -- never a brick.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"
#include "link_proto.h"

/* Binary frame (the only non-JSON traffic on the link): a 12-byte header
 * [u32 seq][u32 len][u32 crc32], little-endian, then len payload bytes.
 * 448 + 12 = 460 B fits the link's 512 B RX ring buffer, so a frame that
 * arrives while the reader is still writing the previous one to flash is
 * buffered, not dropped. seq starts at 1; the host sends frame N+1 only after
 * the oack for N (stop-and-wait: flash writes freeze the single CPU core, so
 * anything pipelined would need a larger ring at the cost of heap). */
#define OTA_FRAME_HDR       12
#define OTA_FRAME_PAYLOAD   448
#define OTA_IDLE_TIMEOUT_MS 5000 /* no bytes this long mid-transfer: host gone */
#define OTA_CONFIRM_S       10   /* boot self-check window before self-confirm */
#define OTA_DONE_REBOOT_MS  5000 /* grace after the final oack, then reboot */

/* True between a successful ota_begin() and the transfer's end/abort: the
 * link reader then feeds every received byte to ota_rx() instead of parsing
 * command lines. */
bool ota_active(void);

/* Writer-side oack poster (link.c's queue), registered once at link start. */
void ota_set_ack_cb(void (*post)(const link_oack_t *ack));

/* Gate + start. Refuses while a band is in DETECT_ALARM (an alarm must not
 * be interrupted), parks the sweep, then erases the target slot -- the erase
 * freezes the CPU for seconds, so hosts must allow ~60 s for the begin ack.
 * *err is a stable string for the ack on failure. */
esp_err_t ota_begin(int id, uint32_t image_size, const char **err);

/* Reader-side byte pump: assembles frames, checks seq + CRC-32, writes them
 * and posts one oack per frame. buf == NULL is the idle-timeout kick: abort
 * the transfer and resume the sweep. Runs in the link reader task. */
void ota_rx(const uint8_t *buf, size_t len);

/* ota_abort command: cancel, abort the OTA handle, resume the sweep. */
void ota_abort(void);

/* ota_reboot command; also invoked automatically OTA_DONE_REBOOT_MS after a
 * completed transfer (so the final oack drains even if the host is gone). */
void ota_reboot(uint32_t delay_ms);

/* ota_confirm command: mark the running image valid now (no-op if it
 * already is). ESP_ERR_NOT_FOUND when there is no otadata (factory build). */
esp_err_t ota_confirm(void);

/* Boot-time self-check: if the running image is PENDING_VERIFY, a task waits
 * OTA_CONFIRM_S and confirms it only if the sweep is alive by then. A crash
 * or hang before that leaves the image pending, and the bootloader boots the
 * previous slot on the next reset. No-op on a factory build. */
void ota_boot_check(void);

/* Slot + image state for hello's "ota" field; false when running from the
 * factory partition (nothing to report). */
bool ota_info(link_ota_info_t *out);
