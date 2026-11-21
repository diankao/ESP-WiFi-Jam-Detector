/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Alarm beeps:
 *   - Touch-LCD-3.5: ES8311 codec + NS4150B amplifier over I2S.
 *   - C5-MINI-KIT-U: optional passive piezo buzzer driven by hardware LEDC PWM
 *     (zero I2S/DMA overhead, < 2 us register toggle).
 */
#include "sdkconfig.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "esp_check.h"
#include "esp_log.h"
#include "audio.h"

static const char *TAG = "audio";

typedef struct {
    uint16_t count, on_ms, off_ms;
} pattern_t;

static QueueHandle_t s_queue;

#if CONFIG_JD_BOARD_C5_MINI_KIT

#include "driver/ledc.h"

#define BUZZER_TASK_PRIO  2
#define BUZZER_TASK_STACK 2048
#define BUZZER_DUTY_ON    128 /* 50% of 8-bit LEDC resolution */

#if CONFIG_JD_BUZZER_GPIO >= 0
static void buzzer_set(bool on)
{
    ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, on ? BUZZER_DUTY_ON : 0);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0);
}

static void buzzer_task(void *arg)
{
    (void)arg;
    pattern_t p;
    for (;;) {
        if (xQueueReceive(s_queue, &p, portMAX_DELAY) != pdTRUE) {
            continue;
        }
        for (unsigned i = 0; i < p.count; i++) {
            buzzer_set(true);
            vTaskDelay(pdMS_TO_TICKS(p.on_ms));
            buzzer_set(false);
            if (i + 1 < p.count && p.off_ms) {
                vTaskDelay(pdMS_TO_TICKS(p.off_ms));
            }
        }
    }
}
#endif

esp_err_t audio_init(void)
{
#if CONFIG_JD_BUZZER_GPIO >= 0
    const ledc_timer_config_t tcfg = {
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .duty_resolution = LEDC_TIMER_8_BIT,
        .timer_num = LEDC_TIMER_0,
        .freq_hz = CONFIG_JD_BUZZER_FREQ_HZ,
        .clk_cfg = LEDC_AUTO_CLK,
    };
    ESP_RETURN_ON_ERROR(ledc_timer_config(&tcfg), TAG, "ledc timer");
    const ledc_channel_config_t ccfg = {
        .gpio_num = CONFIG_JD_BUZZER_GPIO,
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .channel = LEDC_CHANNEL_0,
        .timer_sel = LEDC_TIMER_0,
        .duty = 0,
        .hpoint = 0,
    };
    ESP_RETURN_ON_ERROR(ledc_channel_config(&ccfg), TAG, "ledc channel");
    s_queue = xQueueCreate(1, sizeof(pattern_t));
    ESP_RETURN_ON_FALSE(s_queue, ESP_ERR_NO_MEM, TAG, "queue");
    ESP_RETURN_ON_FALSE(xTaskCreate(buzzer_task, "buzzer", BUZZER_TASK_STACK, NULL, BUZZER_TASK_PRIO, NULL) == pdPASS,
                        ESP_ERR_NO_MEM, TAG, "task");
    ESP_LOGI(TAG, "LEDC PWM buzzer ready on GPIO%d (%d Hz)", CONFIG_JD_BUZZER_GPIO, CONFIG_JD_BUZZER_FREQ_HZ);
#else
    ESP_LOGI(TAG, "buzzer disabled (CONFIG_JD_BUZZER_GPIO < 0)");
#endif
    return ESP_OK;
}

#else /* !CONFIG_JD_BOARD_C5_MINI_KIT */

#include "driver/i2s_std.h"
#include "esp_codec_dev.h"
#include "esp_codec_dev_defaults.h"
#include "board.h"
#include "board_pins.h"

#define RATE_HZ    16000
#define TONE_HZ    2000  /* RATE_HZ / 8: one period is 8 samples */
#define VOLUME     85
#define AMPLITUDE  12000
#define RAMP_MS    5     /* fade in/out, no clicks */
#define CHUNK      256   /* samples per write */
/* Above the sweep: the task only feeds DMA and sleeps, but at a low
 * priority the CPU-bound sweep would starve it and the tone would stutter. */
#define TASK_PRIO  11
#define TASK_STACK 3072
#define PA_SETTLE_MS 10

static esp_codec_dev_handle_t s_codec;

/* sin(2 pi k / 8) * 2^15 */
static const int16_t s_sine8[8] = {0, 23170, 32767, 23170, 0, -23170, -32767, -23170};

static esp_err_t play_tone(unsigned ms)
{
    static int16_t buf[CHUNK];
    const unsigned total = RATE_HZ * ms / 1000, ramp = RATE_HZ * RAMP_MS / 1000;
    for (unsigned done = 0; done < total;) {
        const unsigned n = total - done < CHUNK ? total - done : CHUNK;
        for (unsigned i = 0; i < n; i++) {
            const unsigned k = done + i, edge = k < total - k ? k : total - 1 - k;
            const int32_t a = edge < ramp ? AMPLITUDE * (int32_t)edge / (int32_t)ramp : AMPLITUDE;
            buf[i] = (int16_t)(s_sine8[k % 8] * a / 32768);
        }
        ESP_RETURN_ON_FALSE(esp_codec_dev_write(s_codec, buf, (int)(n * sizeof(int16_t))) == ESP_CODEC_DEV_OK,
                            ESP_FAIL, TAG, "write");
        done += n;
    }
    return ESP_OK;
}

static void silence(unsigned ms)
{
    static const int16_t zero[CHUNK];
    for (unsigned left = RATE_HZ * ms / 1000; left;) {
        const unsigned n = left < CHUNK ? left : CHUNK;
        esp_codec_dev_write(s_codec, (void *)zero, (int)(n * sizeof(int16_t)));
        left -= n;
    }
}

static void audio_task(void *arg)
{
    (void)arg;
    pattern_t p;
    for (;;) {
        if (xQueueReceive(s_queue, &p, portMAX_DELAY) != pdTRUE) {
            continue;
        }
        esp_codec_dev_sample_info_t fs = {.bits_per_sample = 16, .channel = 1, .sample_rate = RATE_HZ};
        if (esp_codec_dev_open(s_codec, &fs) != ESP_CODEC_DEV_OK) {
            ESP_LOGW(TAG, "codec open failed");
            continue;
        }
        esp_codec_dev_set_out_vol(s_codec, VOLUME);
        esp_err_t err = board_pa_enable(true);
        vTaskDelay(pdMS_TO_TICKS(PA_SETTLE_MS));
        for (unsigned i = 0; i < p.count && err == ESP_OK; i++) {
            err = play_tone(p.on_ms);
            silence(i + 1 < p.count ? p.off_ms : 20); /* also flushes the DMA tail */
        }
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "beep: %s", esp_err_to_name(err));
        }
        board_pa_enable(false);
        esp_codec_dev_close(s_codec);
    }
}

esp_err_t audio_init(void)
{
    i2s_chan_handle_t tx;
    i2s_chan_config_t chan = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    ESP_RETURN_ON_ERROR(i2s_new_channel(&chan, &tx, NULL), TAG, "i2s channel");
    const i2s_std_config_t std = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(RATE_HZ),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_MONO),
        .gpio_cfg = {
            .mclk = BOARD_I2S_MCLK,
            .bclk = BOARD_I2S_BCLK,
            .ws = BOARD_I2S_WS,
            .dout = BOARD_I2S_DOUT,
            .din = I2S_GPIO_UNUSED, /* playback only */
        },
    };
    ESP_RETURN_ON_ERROR(i2s_channel_init_std_mode(tx, &std), TAG, "i2s std mode");

    /* esp_codec_dev takes non-const configs */
    audio_codec_i2s_cfg_t i2s_cfg = {.port = I2S_NUM_0, .tx_handle = tx};
    const audio_codec_data_if_t *data_if = audio_codec_new_i2s_data(&i2s_cfg);
    audio_codec_i2c_cfg_t i2c_cfg = {
        .port = BOARD_I2C_PORT,
        .addr = ES8311_CODEC_DEFAULT_ADDR,
        .bus_handle = board_i2c_bus(),
    };
    const audio_codec_ctrl_if_t *ctrl_if = audio_codec_new_i2c_ctrl(&i2c_cfg);
    const audio_codec_gpio_if_t *gpio_if = audio_codec_new_gpio();
    ESP_RETURN_ON_FALSE(data_if && ctrl_if && gpio_if, ESP_ERR_NO_MEM, TAG, "codec interfaces");
    es8311_codec_cfg_t es_cfg = {
        .ctrl_if = ctrl_if,
        .gpio_if = gpio_if,
        .codec_mode = ESP_CODEC_DEV_WORK_MODE_DAC,
        .pa_pin = -1, /* PA is on the expander: board_pa_enable() */
        .master_mode = false,
        .use_mclk = false,
        .hw_gain = {.pa_voltage = 5.0, .codec_dac_voltage = 3.3},
    };
    const audio_codec_if_t *es = es8311_codec_new(&es_cfg);
    ESP_RETURN_ON_FALSE(es, ESP_FAIL, TAG, "ES8311 not found");
    esp_codec_dev_cfg_t dev_cfg = {.dev_type = ESP_CODEC_DEV_TYPE_OUT, .codec_if = es, .data_if = data_if};
    s_codec = esp_codec_dev_new(&dev_cfg);
    ESP_RETURN_ON_FALSE(s_codec, ESP_ERR_NO_MEM, TAG, "codec device");

    s_queue = xQueueCreate(1, sizeof(pattern_t));
    ESP_RETURN_ON_FALSE(s_queue, ESP_ERR_NO_MEM, TAG, "queue");
    ESP_RETURN_ON_FALSE(xTaskCreate(audio_task, "audio", TASK_STACK, NULL, TASK_PRIO, NULL) == pdPASS, ESP_ERR_NO_MEM,
                        TAG, "task");
    ESP_LOGI(TAG, "ES8311 ready (%u Hz tone, volume %u)", TONE_HZ, VOLUME);
    return ESP_OK;
}

#endif /* CONFIG_JD_BOARD_C5_MINI_KIT */

void audio_beep(unsigned count, unsigned on_ms, unsigned off_ms)
{
    if (!s_queue) {
        return;
    }
    const pattern_t p = {(uint16_t)count, (uint16_t)on_ms, (uint16_t)off_ms};
    xQueueSend(s_queue, &p, 0);
}
