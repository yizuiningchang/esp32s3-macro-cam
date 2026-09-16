/**
 * @file  app_config.c
 * @brief NVS 参数配置模块实现
 */
#include "app_config.h"

#include <string.h>
#include "esp_camera.h"   /* 取 framesize_t / FRAMESIZE_UXGA 等枚举值 */
#include "esp_check.h"
#include "esp_log.h"
#include "nvs.h"
#include "nvs_flash.h"

static const char *TAG = "cfg";

#define CFG_NAMESPACE "crackcam"

#define DEF_JPEG_QUALITY  12
/* framesize_t 取值随 esp32-camera 版本变化，不要硬编码数字。
 * 用 (uint8_t)FRAMESIZE_UXGA 让编译器从枚举取值（当前 v2.1.7 下为 15 = 1600x1200）。
 * 注意：旧版 esp32-camera 里 UXGA 是 13，中间插入过 128X128 / 320X320 两档。 */
#define DEF_FRAME_SIZE    ((uint8_t)FRAMESIZE_UXGA)
/* Phase 1 定焦模组：无对焦动作，按下即拍。
 * AF 模组到货后经串口 `cfg set af_settle_ms 500` 启用（键保留）。
 * ⚠ 改此默认值后必须清一次 NVS 才生效：`esptool erase_region 0x9000 0x6000`
 *   （load_*() 只在键不存在时写默认值，旧值会一直留在 NVS 里，见 DEVELOPMENT_NOTES 4.6） */
#define DEF_AF_SETTLE_MS  0
#define DEF_LOW_SPACE_PCT 5
#define DEF_FILE_SEQ      1
#define DEF_AUDIO_VOL     65
#define DEF_AUDIO_MUTE    0
/* 统一保持唤醒时长（= 会话"无操作超时"，v1.6）：默认 5 分钟。
 * 上电 / 唤醒 / 每次拍照（成功或失败）都重置计时；无操作满此时限 → 前 10 s 播「即将休眠」→ 播完入睡。
 * 量产形态支持修改（现经串口 `cfg set hold_awake_s <秒>`，NVS 持久化）。
 * ⚠ 改此默认值后必须清一次 NVS（键已存在时不会覆盖），见 DEVELOPMENT_NOTES 4.6 */
#define DEF_HOLD_AWAKE_S  300

static nvs_handle_t s_nvs;
static app_cfg_t    s_cfg;

static const char *KEY_JPEG_QUALITY  = "jpeg_quality";
static const char *KEY_FRAME_SIZE    = "frame_size";
static const char *KEY_AF_SETTLE_MS  = "af_settle_ms";
static const char *KEY_LOW_SPACE_PCT = "low_space_pct";
static const char *KEY_FILE_SEQ      = "file_seq";
static const char *KEY_AUDIO_VOL     = "audio_vol";
static const char *KEY_AUDIO_MUTE    = "audio_mute";
static const char *KEY_HOLD_AWAKE_S  = "hold_awake_s";

/** 读 u8，缺失/类型不符时写入默认值 */
static uint8_t load_u8(const char *key, uint8_t def)
{
    uint8_t v = def;
    if (nvs_get_u8(s_nvs, key, &v) != ESP_OK) {
        v = def;
        nvs_set_u8(s_nvs, key, v);
        ESP_LOGI(TAG, "写入默认 %s=%u", key, (unsigned)v);
    }
    return v;
}

static uint16_t load_u16(const char *key, uint16_t def)
{
    uint16_t v = def;
    if (nvs_get_u16(s_nvs, key, &v) != ESP_OK) {
        v = def;
        nvs_set_u16(s_nvs, key, v);
        ESP_LOGI(TAG, "写入默认 %s=%u", key, (unsigned)v);
    }
    return v;
}

static uint32_t load_u32(const char *key, uint32_t def)
{
    uint32_t v = def;
    if (nvs_get_u32(s_nvs, key, &v) != ESP_OK) {
        v = def;
        nvs_set_u32(s_nvs, key, v);
        ESP_LOGI(TAG, "写入默认 %s=%u", key, (unsigned)v);
    }
    return v;
}

esp_err_t app_config_init(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "NVS 需重新初始化（%s），执行擦除", esp_err_to_name(err));
        ESP_RETURN_ON_ERROR(nvs_flash_erase(), TAG, "nvs_flash_erase 失败");
        err = nvs_flash_init();
    }
    ESP_RETURN_ON_ERROR(err, TAG, "nvs_flash_init 失败");

    ESP_RETURN_ON_ERROR(nvs_open(CFG_NAMESPACE, NVS_READWRITE, &s_nvs), TAG, "nvs_open 失败");

    s_cfg.jpeg_quality  = load_u8(KEY_JPEG_QUALITY, DEF_JPEG_QUALITY);
    s_cfg.frame_size    = load_u8(KEY_FRAME_SIZE, DEF_FRAME_SIZE);
    s_cfg.af_settle_ms  = load_u16(KEY_AF_SETTLE_MS, DEF_AF_SETTLE_MS);
    s_cfg.low_space_pct = load_u8(KEY_LOW_SPACE_PCT, DEF_LOW_SPACE_PCT);
    s_cfg.file_seq      = load_u32(KEY_FILE_SEQ, DEF_FILE_SEQ);
    s_cfg.audio_vol     = load_u8(KEY_AUDIO_VOL, DEF_AUDIO_VOL);
    s_cfg.audio_mute    = load_u8(KEY_AUDIO_MUTE, DEF_AUDIO_MUTE);
    s_cfg.hold_awake_s  = load_u16(KEY_HOLD_AWAKE_S, DEF_HOLD_AWAKE_S);

    /* 若本轮写入了默认值，统一提交一次 */
    nvs_commit(s_nvs);

    ESP_LOGI(TAG, "配置载入完成：quality=%u frame_size=%u af_settle=%ums low_space=%u%% seq=%u vol=%u mute=%u hold_awake=%us",
             (unsigned)s_cfg.jpeg_quality, (unsigned)s_cfg.frame_size,
             (unsigned)s_cfg.af_settle_ms, (unsigned)s_cfg.low_space_pct,
             (unsigned)s_cfg.file_seq, (unsigned)s_cfg.audio_vol,
             (unsigned)s_cfg.audio_mute, (unsigned)s_cfg.hold_awake_s);
    return ESP_OK;
}

const app_cfg_t *app_config_get(void)
{
    return &s_cfg;
}

esp_err_t app_config_set(const char *key, uint32_t value)
{
    if (key == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t err = ESP_ERR_INVALID_ARG;
    if (strcmp(key, KEY_JPEG_QUALITY) == 0) {
        s_cfg.jpeg_quality = (uint8_t)value;
        err = nvs_set_u8(s_nvs, KEY_JPEG_QUALITY, s_cfg.jpeg_quality);
    } else if (strcmp(key, KEY_FRAME_SIZE) == 0) {
        s_cfg.frame_size = (uint8_t)value;
        err = nvs_set_u8(s_nvs, KEY_FRAME_SIZE, s_cfg.frame_size);
    } else if (strcmp(key, KEY_AF_SETTLE_MS) == 0) {
        s_cfg.af_settle_ms = (uint16_t)value;
        err = nvs_set_u16(s_nvs, KEY_AF_SETTLE_MS, s_cfg.af_settle_ms);
    } else if (strcmp(key, KEY_LOW_SPACE_PCT) == 0) {
        s_cfg.low_space_pct = (uint8_t)value;
        err = nvs_set_u8(s_nvs, KEY_LOW_SPACE_PCT, s_cfg.low_space_pct);
    } else if (strcmp(key, KEY_AUDIO_VOL) == 0) {
        s_cfg.audio_vol = (uint8_t)value;
        err = nvs_set_u8(s_nvs, KEY_AUDIO_VOL, s_cfg.audio_vol);
    } else if (strcmp(key, KEY_AUDIO_MUTE) == 0) {
        s_cfg.audio_mute = (uint8_t)value;
        err = nvs_set_u8(s_nvs, KEY_AUDIO_MUTE, s_cfg.audio_mute);
    } else if (strcmp(key, KEY_HOLD_AWAKE_S) == 0) {
        s_cfg.hold_awake_s = (uint16_t)value;
        err = nvs_set_u16(s_nvs, KEY_HOLD_AWAKE_S, s_cfg.hold_awake_s);
    } else {
        ESP_LOGE(TAG, "未知配置键：%s", key);
        return ESP_ERR_INVALID_ARG;
    }

    ESP_RETURN_ON_ERROR(err, TAG, "写入 NVS 失败");
    ESP_RETURN_ON_ERROR(nvs_commit(s_nvs), TAG, "nvs_commit 失败");
    ESP_LOGI(TAG, "配置已更新：%s=%u", key, (unsigned)value);
    return ESP_OK;
}

esp_err_t app_config_next_file_seq(uint32_t *out)
{
    if (out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    *out = s_cfg.file_seq;

    /* 序号 +1 并持久化，避免掉电后重号覆盖历史照片 */
    s_cfg.file_seq += 1;
    ESP_RETURN_ON_ERROR(nvs_set_u32(s_nvs, KEY_FILE_SEQ, s_cfg.file_seq), TAG, "写序号失败");
    return nvs_commit(s_nvs);
}

esp_err_t app_config_set_file_seq(uint32_t seq)
{
    s_cfg.file_seq = seq;
    ESP_RETURN_ON_ERROR(nvs_set_u32(s_nvs, KEY_FILE_SEQ, s_cfg.file_seq), TAG, "写序号失败");
    return nvs_commit(s_nvs);
}

esp_err_t app_config_reset(void)
{
    esp_err_t err = nvs_erase_all(s_nvs);
    if (err != ESP_OK) {
        return err;
    }
    err = nvs_commit(s_nvs);
    if (err != ESP_OK) {
        return err;
    }

    s_cfg.jpeg_quality  = DEF_JPEG_QUALITY;
    s_cfg.frame_size    = DEF_FRAME_SIZE;
    s_cfg.af_settle_ms  = DEF_AF_SETTLE_MS;
    s_cfg.low_space_pct = DEF_LOW_SPACE_PCT;
    s_cfg.file_seq      = DEF_FILE_SEQ;
    s_cfg.audio_vol     = DEF_AUDIO_VOL;
    s_cfg.audio_mute    = DEF_AUDIO_MUTE;
    s_cfg.hold_awake_s  = DEF_HOLD_AWAKE_S;

    ESP_LOGI(TAG, "配置已恢复默认");
    return ESP_OK;
}
