/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Minimal board support for the Waveshare ESP32-C5-MINI-KIT-U (headless,
 * no LCD / touch / I2C / PMU / TF card). Provides a lightweight radio-quiet
 * mutex so USB Serial/JTAG output and WS2812B LED refreshes never overlap
 * RF ADC captures.
 */
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "esp_check.h"
#include "esp_log.h"
#include "board.h"

static const char *TAG = "board";

static SemaphoreHandle_t s_rf_quiet;

esp_err_t board_init(board_orient_t orient)
{
    (void)orient;
    s_rf_quiet = xSemaphoreCreateMutex();
    ESP_RETURN_ON_FALSE(s_rf_quiet, ESP_ERR_NO_MEM, TAG, "rf_quiet mutex");
    ESP_LOGI(TAG, "Waveshare ESP32-C5-MINI-KIT-U (headless) ready");
    return ESP_OK;
}

esp_err_t board_i2c_scan(void)
{
    return ESP_OK;
}

esp_err_t board_pmu_log(void)
{
    return ESP_OK;
}

esp_err_t board_power_read(board_power_t *out)
{
    if (!out) {
        return ESP_ERR_INVALID_ARG;
    }
    *out = (board_power_t){
        .vbus = true,
        .battery = false,
        .charging = false,
        .percent = -1,
        .battery_mv = 0,
    };
    return ESP_OK;
}

esp_err_t board_pa_enable(bool on)
{
    (void)on;
    return ESP_OK;
}

i2c_master_bus_handle_t board_i2c_bus(void)
{
    return NULL;
}

esp_err_t board_backlight_set(uint8_t percent)
{
    (void)percent;
    return ESP_OK;
}

esp_err_t board_lcd_set_orient(board_orient_t orient)
{
    (void)orient;
    return ESP_OK;
}

board_orient_t board_lcd_orient(void)
{
    return BOARD_ORIENT_LANDSCAPE;
}

esp_err_t board_lcd_draw(int x0, int y0, int x1, int y1, const void *px)
{
    (void)x0;
    (void)y0;
    (void)x1;
    (void)y1;
    (void)px;
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t board_lcd_draw_async(int x0, int y0, int x1, int y1, const void *px, board_lcd_done_cb_t done, void *ctx)
{
    (void)x0;
    (void)y0;
    (void)x1;
    (void)y1;
    (void)px;
    (void)done;
    (void)ctx;
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t board_rf_quiet_begin(uint32_t timeout_ms)
{
    if (!s_rf_quiet) {
        return ESP_OK;
    }
    return xSemaphoreTake(s_rf_quiet, pdMS_TO_TICKS(timeout_ms)) == pdTRUE ? ESP_OK : ESP_ERR_TIMEOUT;
}

void board_rf_quiet_end(void)
{
    if (s_rf_quiet) {
        xSemaphoreGive(s_rf_quiet);
    }
}

esp_err_t board_touch_read(board_touch_t *t)
{
    if (t) {
        *t = (board_touch_t){0};
    }
    return ESP_ERR_NOT_SUPPORTED;
}

bool board_boot_pressed(void)
{
    return false;
}
