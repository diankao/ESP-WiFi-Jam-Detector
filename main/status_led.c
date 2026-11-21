/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Onboard WS2812B RGB status indicator for Waveshare ESP32-C5-MINI-KIT-U.
 *
 * Uses cycle-accurate GPIO bit-banging (esp_cpu_get_cycle_count) instead of
 * the RMT driver because the ESP-IDF 6.2 snapshot pinned by esp-sdr exhibits
 * RMT TX flush timeouts on ESP32-C5. Each 24-bit update takes ~30 us and runs
 * inside board_rf_quiet_begin()/end() so it never overlaps an RF capture.
 *
 * Visual states (highest priority across active bands):
 *   AUTORANGE : purple fast blink
 *   ALARM     : red rapid strobe (+ hardware PWM buzzer if enabled)
 *   SUSPECT   : amber medium blink
 *   LEARNING  : blue smooth breathing
 *   NORMAL    : soft green glow with periodic heartbeat
 */
#include <stdbool.h>
#include <stdint.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "esp_check.h"
#include "esp_cpu.h"
#include "esp_log.h"
#include "esp_private/esp_clk.h"
#include "esp_rom_sys.h"
#include "esp_timer.h"
#include "soc/gpio_struct.h"
#include "audio.h"
#include "board.h"
#include "detector.h"
#include "settings.h"
#include "status_led.h"
#include "sweep.h"

static const char *TAG = "status_led";

#define TASK_PRIO       2
#define TASK_STACK      2560
#define TICK_MS         50
#define ALARM_BEEP_MS   4000

typedef enum {
    LED_NORMAL = 0,
    LED_LEARNING,
    LED_SUSPECT,
    LED_ALARM,
    LED_AUTORANGE,
    LED_FAILED,
} led_mode_t;

#if CONFIG_JD_RGB_LED_GPIO >= 0
static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
static uint32_t s_t0h, s_t0l, s_t1h, s_t1l;

static inline void delay_cycles(uint32_t start, uint32_t cycles)
{
    while ((uint32_t)(esp_cpu_get_cycle_count() - start) < cycles) {
    }
}

static void ws2812_init(void)
{
    const uint32_t cpu_hz = esp_clk_cpu_freq();
    s_t0h = (uint32_t)((uint64_t)cpu_hz * 350ULL / 1000000000ULL);
    s_t0l = (uint32_t)((uint64_t)cpu_hz * 900ULL / 1000000000ULL);
    s_t1h = (uint32_t)((uint64_t)cpu_hz * 800ULL / 1000000000ULL);
    s_t1l = (uint32_t)((uint64_t)cpu_hz * 450ULL / 1000000000ULL);

    gpio_reset_pin(CONFIG_JD_RGB_LED_GPIO);
    gpio_set_direction(CONFIG_JD_RGB_LED_GPIO, GPIO_MODE_OUTPUT);
    gpio_set_drive_capability(CONFIG_JD_RGB_LED_GPIO, GPIO_DRIVE_CAP_3);
    gpio_set_level(CONFIG_JD_RGB_LED_GPIO, 0);
    esp_rom_delay_us(100);
}

static void ws2812_set(uint8_t r, uint8_t g, uint8_t b)
{
    if (board_rf_quiet_begin(50) != ESP_OK) {
        return; /* skip this frame rather than delaying a capture */
    }
    const uint32_t grb = ((uint32_t)g << 16) | ((uint32_t)r << 8) | (uint32_t)b;
    const uint32_t mask = 1U << CONFIG_JD_RGB_LED_GPIO;

    portENTER_CRITICAL(&s_mux);
    for (int i = 23; i >= 0; i--) {
        const uint32_t bit = (grb >> i) & 1U;
        const uint32_t hi = bit ? s_t1h : s_t0h;
        const uint32_t lo = bit ? s_t1l : s_t0l;

        const uint32_t t0 = esp_cpu_get_cycle_count();
        GPIO.out_w1ts.val = mask;
        delay_cycles(t0, hi);

        const uint32_t t1 = esp_cpu_get_cycle_count();
        GPIO.out_w1tc.val = mask;
        delay_cycles(t1, lo);
    }
    portEXIT_CRITICAL(&s_mux);
    board_rf_quiet_end();
    esp_rom_delay_us(80);
}
#endif

static led_mode_t sample_mode(void)
{
    const sweep_state_t sw = sweep_state();
    if (sw == SWEEP_FAILED) {
        return LED_FAILED;
    }
    if (sw == SWEEP_AUTORANGE || sw == SWEEP_IDLE) {
        return LED_AUTORANGE;
    }
    led_mode_t best = LED_NORMAL;
    for (int b = 0; b < BAND_COUNT; b++) {
        if (!sweep_scanned((band_id_t)b)) {
            continue;
        }
        detect_status_t st;
        if (detector_get_status((band_id_t)b, &st) != ESP_OK) {
            continue;
        }
        led_mode_t m = LED_NORMAL;
        switch (st.state) {
        case DETECT_ALARM:    m = LED_ALARM; break;
        case DETECT_SUSPECT:  m = LED_SUSPECT; break;
        case DETECT_LEARNING: m = LED_LEARNING; break;
        case DETECT_NORMAL:   m = LED_NORMAL; break;
        }
        if (m > best) {
            best = m;
        }
    }
    return best;
}

static void render_rgb(led_mode_t mode, uint32_t t_ms, uint8_t *r, uint8_t *g, uint8_t *b)
{
    *r = *g = *b = 0;
    switch (mode) {
    case LED_AUTORANGE:
        /* Purple fast blink (200 ms period). */
        if ((t_ms / 100u) & 1u) {
            *r = 0x48;
            *b = 0x48;
        }
        break;
    case LED_LEARNING: {
        /* Blue smooth triangle breathing (2000 ms period). */
        const uint32_t phase = t_ms % 2000u;
        const uint32_t tri = phase < 1000u ? phase : (2000u - phase);
        *b = (uint8_t)(4u + (tri * 72u) / 1000u);
        break;
    }
    case LED_NORMAL: {
        /* Soft green glow with a brief brighter heartbeat every 3 s. */
        const uint32_t phase = t_ms % 3000u;
        *g = (phase < 150u) ? 0x30 : 0x0C;
        break;
    }
    case LED_SUSPECT:
        /* Amber medium blink (400 ms period). */
        if ((t_ms / 200u) & 1u) {
            *r = 0x60;
            *g = 0x2C;
        }
        break;
    case LED_ALARM:
        /* Red rapid strobe (150 ms period). */
        if ((t_ms / 75u) & 1u) {
            *r = 0x80;
        }
        break;
    case LED_FAILED:
        /* Magenta/red slow blink. */
        if ((t_ms / 500u) & 1u) {
            *r = 0x60;
            *b = 0x10;
        }
        break;
    }
}

static void status_led_task(void *arg)
{
    (void)arg;
    uint8_t last_r = 0xFF, last_g = 0xFF, last_b = 0xFF;
    led_mode_t prev_mode = LED_AUTORANGE;
    uint32_t last_beep_ms = 0;

    for (;;) {
        const uint32_t t_ms = (uint32_t)(esp_timer_get_time() / 1000);
        const led_mode_t mode = sample_mode();

        /* Trigger hardware PWM buzzer when entering or sustaining ALARM. */
        if (mode == LED_ALARM) {
            if (prev_mode != LED_ALARM || (t_ms - last_beep_ms >= ALARM_BEEP_MS)) {
                settings_t set;
                settings_get(&set);
                if (set.beep) {
                    audio_beep(3, 120, 80);
                }
                last_beep_ms = t_ms;
            }
        }
        prev_mode = mode;

#if CONFIG_JD_RGB_LED_GPIO >= 0
        uint8_t r, g, b;
        render_rgb(mode, t_ms, &r, &g, &b);
        if (r != last_r || g != last_g || b != last_b) {
            ws2812_set(r, g, b);
            last_r = r;
            last_g = g;
            last_b = b;
        }
#else
        (void)last_r;
        (void)last_g;
        (void)last_b;
        (void)render_rgb;
#endif
        vTaskDelay(pdMS_TO_TICKS(TICK_MS));
    }
}

esp_err_t status_led_start(void)
{
#if CONFIG_JD_RGB_LED_GPIO >= 0
    ws2812_init();
    ws2812_set(0x48, 0x00, 0x48); /* immediate purple on boot */
    ESP_LOGI(TAG, "onboard WS2812B RGB LED on GPIO%d ready", CONFIG_JD_RGB_LED_GPIO);
#else
    ESP_LOGI(TAG, "RGB LED disabled (CONFIG_JD_RGB_LED_GPIO < 0)");
#endif
    ESP_RETURN_ON_FALSE(xTaskCreate(status_led_task, "status_led", TASK_STACK, NULL, TASK_PRIO, NULL) == pdPASS,
                        ESP_ERR_NO_MEM, TAG, "task");
    return ESP_OK;
}
