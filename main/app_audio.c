/**
 * @file  app_audio.c
 * @brief 声音反馈模块实现
 */
#include "app_audio.h"
#include "app_tones.h"
#include "app_config.h"

#include "bsp/esp-bsp.h"
#include "driver/i2s_std.h"
#include "esp_codec_dev.h"
#include "esp_io_expander.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "audio";

/* EXIO 映射以原理图 PIN OUT 表为准：
 * EXIO4 = 功放使能 PA_CTRL（1=开 / 0=关）。官方 Arduino 注释标成 SD CS 是错的。 */
#define EXIO_PA_CTRL  IO_EXPANDER_PIN_NUM_4

/** 功放上电稳定时间（ms），避免开功放瞬间的爆音切掉音头 */
#define PA_SETTLE_MS  10

static esp_codec_dev_handle_t s_spk = NULL;
static bool                   s_closed = false;   /**< 已调用 app_audio_sleep()（codec 已 suspend） */

/** 开/关功放（EXIO4） */
static esp_err_t pa_enable(bool on)
{
    esp_io_expander_handle_t io = bsp_get_io_expander_handle();
    if (io == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    return esp_io_expander_set_level(io, EXIO_PA_CTRL, on ? 1 : 0);
}

/** 告警音（静音时仍保留）：失败 / 卡异常 / 低电量 */
static bool is_alert_tone(tone_id_t tone)
{
    return tone == TONE_FAIL || tone == TONE_SD_ERROR || tone == TONE_LOW_BATTERY;
}

esp_err_t app_audio_init(void)
{
    /* I2S 16 kHz / 16 bit / 单声道，与内嵌 PCM 一致（BSP 默认 22050，这里显式覆盖为 16k） */
    const i2s_std_config_t i2s_cfg = {
        .clk_cfg  = I2S_STD_CLK_DEFAULT_CONFIG(TONE_SAMPLE_RATE),
        .slot_cfg = I2S_STD_PHILIP_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_MONO),
        .gpio_cfg = {
            .mclk = BSP_I2S_MCLK,   /* GPIO10 */
            .bclk = BSP_I2S_SCLK,   /* GPIO11 */
            .ws   = BSP_I2S_LCLK,   /* GPIO12 */
            .dout = BSP_I2S_DOUT,   /* GPIO14 */
            .din  = BSP_I2S_DSIN,   /* GPIO13（麦克风方向，本阶段不用） */
            .invert_flags = {
                .mclk_inv = false,
                .bclk_inv = false,
                .ws_inv   = false,
            },
        },
    };

    esp_err_t err = bsp_audio_init(&i2s_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "I2S 初始化失败：%s", esp_err_to_name(err));
        return err;
    }

    s_spk = bsp_audio_codec_speaker_init();
    if (s_spk == NULL) {
        ESP_LOGE(TAG, "扬声器 codec（ES8311）初始化失败");
        return ESP_FAIL;
    }

    esp_codec_dev_sample_info_t fs = {
        .bits_per_sample = 16,
        .channel = 1,
        .channel_mask = 0,
        .sample_rate = TONE_SAMPLE_RATE,
        .mclk_multiple = 256,
    };
    if (esp_codec_dev_open(s_spk, &fs) != ESP_CODEC_DEV_OK) {
        ESP_LOGE(TAG, "codec open 失败");
        return ESP_FAIL;
    }

    const app_cfg_t *cfg = app_config_get();
    esp_codec_dev_set_out_vol(s_spk, (int)cfg->audio_vol);

    /* 初始功放关断（上电默认关，双保险） */
    pa_enable(false);

    ESP_LOGI(TAG, "音频通路就绪：16 kHz 16 bit 单声道，音量 %u%%", (unsigned)cfg->audio_vol);
    return ESP_OK;
}

esp_err_t app_audio_play(tone_id_t tone)
{
    if (tone >= TONE_COUNT) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_spk == NULL) {
        return ESP_ERR_INVALID_STATE;   /* 音频未初始化/失败，静默降级 */
    }
    if (s_closed) {
        return ESP_ERR_INVALID_STATE;   /* 已进入深睡前低功耗态，不再发声 */
    }

    const app_cfg_t *cfg = app_config_get();
    if (cfg->audio_mute && !is_alert_tone(tone)) {
        ESP_LOGI(TAG, "静音：跳过常规提示音 #%d", (int)tone);
        return ESP_OK;
    }

    const tone_entry_t *e = &g_tone_table[tone];

    esp_err_t err = pa_enable(true);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "开功放失败：%s", esp_err_to_name(err));
        return err;
    }
    vTaskDelay(pdMS_TO_TICKS(PA_SETTLE_MS));

    int w = esp_codec_dev_write(s_spk, (void *)e->pcm, (int)e->bytes);
    if (w != ESP_CODEC_DEV_OK) {
        ESP_LOGW(TAG, "写入 PCM 失败（%d）", w);
    }

    vTaskDelay(pdMS_TO_TICKS(e->ms));   /* 阻塞等待播完 */

    pa_enable(false);
    ESP_LOGI(TAG, "播放提示音 #%d（%lu ms）", (int)tone, (unsigned long)e->ms);
    return ESP_OK;
}

esp_err_t app_audio_power_amp_off(void)
{
    return pa_enable(false);
}

esp_err_t app_audio_sleep(void)
{
    esp_err_t err = pa_enable(false);   /* EXIO4=0，功放使能位（先关，双保险） */

    if (s_spk != NULL && !s_closed) {
        /* esp_codec_dev_close → es8311_close（mute + PA disable + suspend）： 
         * 内部会写 ES8311 的 DAC/ADC/系统寄存器进入 suspend，是"codec 低功耗"的官方路径。 */
        int r = esp_codec_dev_close(s_spk);
        s_closed = true;
        ESP_LOGI(TAG, "ES8311 已置低功耗（codec close ret=%d）", r);
    }
    return err;
}
