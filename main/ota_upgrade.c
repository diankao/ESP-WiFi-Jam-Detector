/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * See ota_upgrade.h for the flow. Everything here runs in the link reader
 * task (the byte pump), except the boot self-check and reboot tasks.
 */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "esp_system.h"
#include "detector.h"
#include "link_proto.h"
#include "ota_upgrade.h"
#include "sweep.h"

static const char *TAG = "ota";

static struct {
    bool active; /* binary frame mode */
    esp_ota_handle_t handle;
    const esp_partition_t *part;
    uint32_t image_size, written, expect_seq;
    uint32_t seq, crc;      /* parsed from the frame header being assembled */
    uint16_t hdr_got, len;  /* header bytes seen / payload length it declared */
    uint16_t pay_got;       /* payload bytes assembled */
    uint8_t hdr[OTA_FRAME_HDR];
} s_ota;
static uint8_t s_frame[OTA_FRAME_PAYLOAD]; /* static: 448 B off the reader stack */
static int s_id;                           /* ota_begin's id, echoed in every oack */
static void (*s_post)(const link_oack_t *);
static bool s_rebooting;

/* CRC-32/ISO-HDLC, bitwise (poly 0xEDB88320, reflected, zlib-compatible, so
 * the host tool can just use zlib.crc32). ~0.1 s of CPU for a whole image on
 * the 240 MHz core: not worth a 1 KB table. */
static uint32_t crc32(const uint8_t *p, size_t n)
{
    uint32_t crc = 0xffffffffu;
    for (size_t i = 0; i < n; i++) {
        crc ^= p[i];
        for (int b = 0; b < 8; b++) {
            crc = (crc >> 1) ^ (0xedb88320u & (0u - (crc & 1u)));
        }
    }
    return crc ^ 0xffffffffu;
}

static uint32_t rd32(const uint8_t *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

static void post_oack(bool ok, const char *err, uint32_t seq, uint32_t bytes, bool done)
{
    if (!s_post) {
        return;
    }
    link_oack_t o = {.ok = ok, .id = s_id, .seq = seq, .bytes = bytes, .err = err, .done = done};
    if (done && s_ota.part) {
        snprintf(o.slot, sizeof(o.slot), "%s", s_ota.part->label);
    }
    s_post(&o);
}

/* Tear the session down and let the sweep run again. The OTA handle must be
 * gone by the time this is called (or abort_handle must be true). */
static void cleanup(bool abort_handle)
{
    if (abort_handle && s_ota.handle) {
        esp_ota_abort(s_ota.handle);
    }
    memset(&s_ota, 0, sizeof(s_ota));
    sweep_pause(false);
}

static void fail(const char *err, uint32_t seq)
{
    ESP_LOGW(TAG, "%s at seq %u (%u/%u B); transfer aborted, sweep resumes", err, (unsigned)seq,
             (unsigned)s_ota.written, (unsigned)s_ota.image_size);
    const uint32_t written = s_ota.written;
    if (s_ota.handle) {
        esp_ota_abort(s_ota.handle);
    }
    post_oack(false, err, seq, written, false);
    cleanup(false);
}

bool ota_active(void)
{
    return s_ota.active;
}

void ota_set_ack_cb(void (*post)(const link_oack_t *ack))
{
    s_post = post;
}

esp_err_t ota_begin(int id, uint32_t image_size, const char **err)
{
    *err = NULL;
    if (s_ota.active) {
        *err = "busy";
        return ESP_ERR_INVALID_STATE;
    }
    if (image_size == 0 || image_size > 4u * 1024 * 1024) {
        *err = "size";
        return ESP_ERR_INVALID_ARG;
    }
    const esp_partition_t *part = esp_ota_get_next_update_partition(NULL);
    if (!part) {
        *err = "no ota"; /* factory-only partition table */
        return ESP_ERR_NOT_SUPPORTED;
    }
    /* Safety gate: never interrupt an alarm in progress (and pausing the
     * sweep would freeze its state machine anyway). The host retries once
     * the band is back to normal. */
    for (int b = 0; b < BAND_COUNT; b++) {
        detect_status_t st;
        detector_get_status((band_id_t)b, &st);
        if (st.state == DETECT_ALARM) {
            *err = "active alarm";
            return ESP_ERR_INVALID_STATE;
        }
    }

    sweep_pause(true);
    ESP_LOGI(TAG, "erasing %s for a %u B image (the CPU freezes for the erase)", part->label,
             (unsigned)image_size);
    esp_err_t r = esp_ota_begin(part, image_size, &s_ota.handle);
    if (r != ESP_OK) {
        sweep_pause(false);
        *err = r == ESP_ERR_INVALID_SIZE ? "too big" : "flash";
        return r;
    }
    s_ota.part = part;
    s_ota.image_size = image_size;
    s_ota.expect_seq = 1;
    s_ota.active = true;
    s_id = id;
    return ESP_OK;
}

/* One fully assembled frame: validate, write, ack; on the last one verify
 * the image and switch the boot slot. Any error aborts the whole transfer
 * (the host restarts from scratch; there is no resume). */
static void frame_done(void)
{
    const uint32_t seq = s_ota.seq, len = s_ota.len;
    if (seq != s_ota.expect_seq) {
        fail("seq", seq);
        return;
    }
    if (s_ota.written + len > s_ota.image_size) {
        fail("size", seq);
        return;
    }
    if (crc32(s_frame, len) != s_ota.crc) {
        fail("crc", seq);
        return;
    }
    if (esp_ota_write(s_ota.handle, s_frame, len) != ESP_OK) {
        fail("flash", seq);
        return;
    }
    s_ota.written += len;
    s_ota.expect_seq++;

    if (s_ota.written < s_ota.image_size) {
        post_oack(true, NULL, seq, s_ota.written, false);
        return;
    }
    /* Image complete: esp_ota_end verifies header, segments and the appended
     * SHA-256 (bootloader-grade integrity; the wire CRC only catches
     * transport corruption), consuming the handle either way. */
    esp_err_t r = esp_ota_end(s_ota.handle);
    s_ota.handle = 0;
    if (r != ESP_OK) {
        fail("verify", seq);
        return;
    }
    r = esp_ota_set_boot_partition(s_ota.part); /* atomic otadata write */
    if (r != ESP_OK) {
        fail("boot", seq);
        return;
    }
    ESP_LOGI(TAG, "image verified, %s boots next as PENDING_VERIFY (rollback armed)", s_ota.part->label);
    post_oack(true, NULL, seq, s_ota.written, true);
    s_ota.active = false; /* keep the sweep parked: reboot is coming */
    ota_reboot(OTA_DONE_REBOOT_MS);
}

void ota_rx(const uint8_t *buf, size_t len)
{
    if (!s_ota.active) {
        return;
    }
    if (!buf || !len) { /* idle timeout from the link reader */
        fail("timeout", s_ota.expect_seq);
        return;
    }
    size_t i = 0;
    while (i < len && s_ota.active) {
        if (s_ota.hdr_got < OTA_FRAME_HDR) {
            const size_t take = (OTA_FRAME_HDR - s_ota.hdr_got <= len - i) ? OTA_FRAME_HDR - s_ota.hdr_got
                                                                           : len - i;
            memcpy(s_ota.hdr + s_ota.hdr_got, buf + i, take);
            s_ota.hdr_got += take;
            i += take;
            if (s_ota.hdr_got == OTA_FRAME_HDR) {
                s_ota.seq = rd32(s_ota.hdr);
                const uint32_t flen = rd32(s_ota.hdr + 4);
                s_ota.crc = rd32(s_ota.hdr + 8);
                if (flen == 0 || flen > OTA_FRAME_PAYLOAD) {
                    fail("size", s_ota.seq);
                    return;
                }
                s_ota.len = (uint16_t)flen;
                s_ota.pay_got = 0;
            }
            continue;
        }
        const size_t take = (s_ota.len - s_ota.pay_got <= len - i) ? s_ota.len - s_ota.pay_got : len - i;
        memcpy(s_frame + s_ota.pay_got, buf + i, take);
        s_ota.pay_got += take;
        i += take;
        if (s_ota.pay_got == s_ota.len) {
            s_ota.hdr_got = 0;
            frame_done();
        }
    }
}

void ota_abort(void)
{
    if (s_ota.active) {
        fail("abort", s_ota.expect_seq);
    }
}

static void reboot_task(void *arg)
{
    vTaskDelay(pdMS_TO_TICKS((uint32_t)(uintptr_t)arg));
    ESP_LOGI(TAG, "rebooting into the upgraded slot");
    esp_restart();
}

void ota_reboot(uint32_t delay_ms)
{
    if (s_rebooting) {
        return;
    }
    s_rebooting = true;
    if (xTaskCreate(reboot_task, "ota_rst", 2048, (void *)(uintptr_t)delay_ms, 2, NULL) != pdPASS) {
        esp_restart(); /* cannot wait; the slot is already switched */
    }
}

esp_err_t ota_confirm(void)
{
    esp_err_t r = esp_ota_mark_app_valid_cancel_rollback();
    if (r == ESP_OK) {
        ESP_LOGI(TAG, "image confirmed valid (host command)");
    }
    return r == ESP_ERR_INVALID_STATE ? ESP_OK : r; /* already valid: fine */
}

static void verify_task(void *arg)
{
    (void)arg;
    vTaskDelay(pdMS_TO_TICKS(OTA_CONFIRM_S * 1000));
    if (sweep_state() != SWEEP_RUNNING) {
        ESP_LOGW(TAG, "sweep not running after %u s: image stays pending, next reset rolls back",
                 OTA_CONFIRM_S);
    } else if (esp_ota_mark_app_valid_cancel_rollback() == ESP_OK) {
        ESP_LOGI(TAG, "%u s self-check passed, image confirmed valid", OTA_CONFIRM_S);
    }
    vTaskDelete(NULL);
}

void ota_boot_check(void)
{
    const esp_partition_t *run = esp_ota_get_running_partition();
    esp_ota_img_states_t st;
    if (!run || esp_ota_get_state_partition(run, &st) != ESP_OK || st != ESP_OTA_IMG_PENDING_VERIFY) {
        return;
    }
    ESP_LOGW(TAG, "running %s is PENDING_VERIFY: %u s self-check (or ota_confirm) before it counts",
             run->label, OTA_CONFIRM_S);
    if (xTaskCreate(verify_task, "ota_verify", 3072, NULL, 2, NULL) != pdPASS) {
        ESP_LOGE(TAG, "cannot start the self-check task; confirming at once");
        esp_ota_mark_app_valid_cancel_rollback();
    }
}

bool ota_info(link_ota_info_t *out)
{
    const esp_partition_t *run = esp_ota_get_running_partition();
    if (!run || !strcmp(run->label, "factory")) {
        return false; /* factory-only build (the LCD board): nothing to report */
    }
    static const char *const names[] = {"new", "valid", "invalid", "aborted", "pending", "unknown"};
    esp_ota_img_states_t st = ESP_OTA_IMG_NEW;
    if (esp_ota_get_state_partition(run, &st) != ESP_OK) {
        st = ESP_OTA_IMG_NEW; /* empty otadata (never upgraded, or flashed over) */
    }
    snprintf(out->slot, sizeof(out->slot), "%s", run->label);
    snprintf(out->img, sizeof(out->img), "%s",
             names[(unsigned)st < sizeof(names) / sizeof(names[0]) ? st : ESP_OTA_IMG_UNDEFINED]);
    return true;
}
