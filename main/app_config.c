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
 * 用 (uint8_t)FRAMESIZE_HD 让编译器从枚举取值（当前 v2.1.7 下为 13 = 1280x720）。
 * 2026-09-24 用户决定默认降为 HD：2x2 binning 档、文件 ~30KB、抓帧更快；
 *   0.3mm 细节@5cm 有 4.3px（要求工作距离 ≤7cm）。
 * 注意：旧版 esp32-camera 里档位值不同，中间插入过 128X128 / 320X320 两档。 */
#define DEF_FRAME_SIZE    ((uint8_t)FRAMESIZE_HD)
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

/* ---- 画质调参默认值（2026-09-24：现场工作参数固化为默认）------------------------
 * 按现场实测（暗场竖纹治理），把"开箱即用"的工作参数固化为默认值。
 * 新设备（空 NVS）首次启动即得到与调优设备一致的画质基线：
 *   gain_ceil = 128（AGC 上限 8x，压暗场竖纹；出厂 248 = 15.5x）
 *   denoise   = 8  （暗场降噪档）
 *   sharpness = 5  （ISP 锐度 +2）
 * 其余仍为"不干预"哨兵（0xFF/0xFFFF）：aec_limit / ae_level / ae_lock / wb_lock。
 * ⚠ NVS 陷阱：旧设备键已存在，改默认值不影响其现值（保持其 NVS 里的值）。
 * ⚠ 回到"出厂不干预"调试态：现场 `cfg set gain_ceil 248` / `cfg set denoise 255` 等。 */
#define DEF_GAIN_CEIL     128     /* 增益码 = 增益×16；128 = 8x（现场工作值） */
#define DEF_AEC_LIMIT     0xFFFF  /* 不干预（出厂 0x3A02:03 = 984 行） */
#define DEF_AE_LEVEL      0xFF    /* 不干预 */
#define DEF_DENOISE       8       /* 现场工作值（暗场降噪；0=关，8=最强） */
#define DEF_AE_LOCK       0       /* 0 = 保持自动曝光/自动增益 */
#define DEF_WB_LOCK       0       /* 0 = 保持自动白平衡 */
#define DEF_SHARPNESS     5       /* ISP 锐度 +2（NVS 0..6 → 驱动 -3..+3，3=中性） */
#define DEF_AEC_VALUE     300     /* 仅 ae_lock=1 时使用 */
#define DEF_AGC_GAIN      0       /* 仅 ae_lock=1 时使用 */

/* ---- 外置补光默认值（2026-09-19）------------------------------------
 * light_en 默认 0 = 不点灯，保证 Phase 1 拍照行为与画质口径完全不变；
 * 灯板接好后再 `cfg set light_en 1` 打开（NVS 持久化，掉电保留）。
 * light_gpio 默认 5 = LCD 18pin 排线座 pin4（原理图 LCD_SCK，无屏时空闲）。 */
#define DEF_LIGHT_EN      0
#define DEF_LIGHT_MODE    0       /* 0=拍照脉冲（省电） */
#define DEF_LIGHT_LEVEL   255     /* 抓帧建议满亮：PWM 波纹不进画面 */
#define DEF_LIGHT_LEAD_MS 200     /* 点亮→抓帧的 AE 收敛等待 */
#define DEF_LIGHT_GPIO    5

/* ---- 参数基线版本（迁移机制，2026-09-28）--------------------------------------
 * 解决"NVS 陷阱"（DEVELOPMENT_NOTES §4.6）：`load_*()` 只在键不存在时写默认值，
 * 于是"旧固件写过的旧参数"会挡住新固件的默认值——迁移/升级时必须人工清 NVS 才生效。
 * 本机制让固件自己完成迁移：启动时若 NVS 中的版本落后（含缺失），
 * 自动重置**画质类参数**为新默认值（只动画质，不动照片序号/会话时长/音量等现场数据）。
 * ⚠ 以后改动画质默认值/基线时，把 CFG_PARAM_VER +1 即可，设备烧录后自动迁移。 */
#define CFG_PARAM_VER     1

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
/* 画质调参键（v1.8） */
static const char *KEY_GAIN_CEIL     = "gain_ceil";
static const char *KEY_AEC_LIMIT     = "aec_limit";
static const char *KEY_AE_LEVEL      = "ae_level";
static const char *KEY_DENOISE       = "denoise";
static const char *KEY_AE_LOCK       = "ae_lock";
static const char *KEY_WB_LOCK       = "wb_lock";
static const char *KEY_SHARPNESS     = "sharpness";
static const char *KEY_AEC_VALUE     = "aec_value";
static const char *KEY_AGC_GAIN      = "agc_gain";
/* 外置补光键 */
static const char *KEY_LIGHT_EN      = "light_en";
static const char *KEY_LIGHT_MODE    = "light_mode";
static const char *KEY_LIGHT_LEVEL   = "light_level";
static const char *KEY_LIGHT_LEAD_MS = "light_lead_ms";
static const char *KEY_LIGHT_GPIO    = "light_gpio";
/* 参数基线版本键（迁移机制） */
static const char *KEY_CFG_VER       = "cfg_ver";

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

/**
 * @brief 参数基线版本迁移：NVS 版本落后时自动重置画质类参数为编译期默认值
 *
 * 触发条件：`cfg_ver` 键不存在（旧固件/新板子）或 < CFG_PARAM_VER。
 * 动作：删除画质类键（后续 load_* 自动写入新默认值）并记录当前版本。
 * 只重置画质类参数，保留 file_seq / hold_awake_s / 音频 / 补光等现场数据。
 */
static void migrate_params_if_needed(void)
{
    uint8_t ver = 0;
    if (nvs_get_u8(s_nvs, KEY_CFG_VER, &ver) == ESP_OK && ver >= CFG_PARAM_VER) {
        return;   /* 已是当前基线，保留现场值 */
    }

    ESP_LOGW(TAG, "参数基线版本 %u → %u：自动重置画质类参数为编译期默认值（迁移机制）",
             (unsigned)ver, (unsigned)CFG_PARAM_VER);

    const char *keys[] = {
        KEY_FRAME_SIZE, KEY_JPEG_QUALITY,
        KEY_GAIN_CEIL, KEY_AEC_LIMIT, KEY_AE_LEVEL, KEY_DENOISE,
        KEY_AE_LOCK, KEY_WB_LOCK, KEY_SHARPNESS, KEY_AEC_VALUE, KEY_AGC_GAIN,
    };
    for (size_t i = 0; i < sizeof(keys) / sizeof(keys[0]); i++) {
        (void)nvs_erase_key(s_nvs, keys[i]);   /* 键不存在返回 NOT_FOUND，属正常 */
    }
    (void)nvs_set_u8(s_nvs, KEY_CFG_VER, CFG_PARAM_VER);
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

    /* 参数基线版本迁移（必须在 load_* 之前）：旧 NVS 自动重置为新默认值，
     * 保证"迁移/升级后烧录即生效"，无需人工清 NVS。 */
    migrate_params_if_needed();

    s_cfg.jpeg_quality  = load_u8(KEY_JPEG_QUALITY, DEF_JPEG_QUALITY);
    s_cfg.frame_size    = load_u8(KEY_FRAME_SIZE, DEF_FRAME_SIZE);
    s_cfg.af_settle_ms  = load_u16(KEY_AF_SETTLE_MS, DEF_AF_SETTLE_MS);
    s_cfg.low_space_pct = load_u8(KEY_LOW_SPACE_PCT, DEF_LOW_SPACE_PCT);
    s_cfg.file_seq      = load_u32(KEY_FILE_SEQ, DEF_FILE_SEQ);
    s_cfg.audio_vol     = load_u8(KEY_AUDIO_VOL, DEF_AUDIO_VOL);
    s_cfg.audio_mute    = load_u8(KEY_AUDIO_MUTE, DEF_AUDIO_MUTE);
    s_cfg.hold_awake_s  = load_u16(KEY_HOLD_AWAKE_S, DEF_HOLD_AWAKE_S);

    s_cfg.gain_ceil     = load_u16(KEY_GAIN_CEIL, DEF_GAIN_CEIL);
    s_cfg.aec_limit     = load_u16(KEY_AEC_LIMIT, DEF_AEC_LIMIT);
    s_cfg.ae_level      = load_u8(KEY_AE_LEVEL, DEF_AE_LEVEL);
    s_cfg.denoise       = load_u8(KEY_DENOISE, DEF_DENOISE);
    s_cfg.ae_lock       = load_u8(KEY_AE_LOCK, DEF_AE_LOCK);
    s_cfg.wb_lock       = load_u8(KEY_WB_LOCK, DEF_WB_LOCK);
    s_cfg.sharpness     = load_u8(KEY_SHARPNESS, DEF_SHARPNESS);
    s_cfg.aec_value     = load_u16(KEY_AEC_VALUE, DEF_AEC_VALUE);
    s_cfg.agc_gain      = load_u8(KEY_AGC_GAIN, DEF_AGC_GAIN);

    /* 外置补光 */
    s_cfg.light_en      = load_u8(KEY_LIGHT_EN, DEF_LIGHT_EN);
    s_cfg.light_mode    = load_u8(KEY_LIGHT_MODE, DEF_LIGHT_MODE);
    s_cfg.light_level   = load_u8(KEY_LIGHT_LEVEL, DEF_LIGHT_LEVEL);
    s_cfg.light_lead_ms = load_u16(KEY_LIGHT_LEAD_MS, DEF_LIGHT_LEAD_MS);
    s_cfg.light_gpio    = load_u8(KEY_LIGHT_GPIO, DEF_LIGHT_GPIO);

    /* 若本轮写入了默认值，统一提交一次 */
    nvs_commit(s_nvs);

    ESP_LOGI(TAG, "配置载入完成：quality=%u frame_size=%u af_settle=%ums low_space=%u%% seq=%u vol=%u mute=%u hold_awake=%us",
             (unsigned)s_cfg.jpeg_quality, (unsigned)s_cfg.frame_size,
             (unsigned)s_cfg.af_settle_ms, (unsigned)s_cfg.low_space_pct,
             (unsigned)s_cfg.file_seq, (unsigned)s_cfg.audio_vol,
             (unsigned)s_cfg.audio_mute, (unsigned)s_cfg.hold_awake_s);
    ESP_LOGI(TAG, "画质调参：gain_ceil=%u aec_limit=%u ae_level=%u denoise=%u ae_lock=%u wb_lock=%u sharpness=%u aec_value=%u agc_gain=%u（255/65535=不干预）",
             (unsigned)s_cfg.gain_ceil, (unsigned)s_cfg.aec_limit,
             (unsigned)s_cfg.ae_level, (unsigned)s_cfg.denoise, (unsigned)s_cfg.ae_lock, (unsigned)s_cfg.wb_lock,
             (unsigned)s_cfg.sharpness,
             (unsigned)s_cfg.aec_value, (unsigned)s_cfg.agc_gain);
    ESP_LOGI(TAG, "补光：light_en=%u mode=%u level=%u lead=%ums gpio=%u",
             (unsigned)s_cfg.light_en, (unsigned)s_cfg.light_mode,
             (unsigned)s_cfg.light_level, (unsigned)s_cfg.light_lead_ms,
             (unsigned)s_cfg.light_gpio);
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
    } else if (strcmp(key, KEY_GAIN_CEIL) == 0) {
        if (value > 1023) { ESP_LOGE(TAG, "gain_ceil 越界（0..1023）：%u", (unsigned)value); return ESP_ERR_INVALID_ARG; }
        s_cfg.gain_ceil = (uint16_t)value;
        err = nvs_set_u16(s_nvs, KEY_GAIN_CEIL, s_cfg.gain_ceil);
    } else if (strcmp(key, KEY_AEC_LIMIT) == 0) {
        if (value != 0xFFFF && value > 1960) { ESP_LOGE(TAG, "aec_limit 越界（0..1960 或 65535=不干预）：%u", (unsigned)value); return ESP_ERR_INVALID_ARG; }
        s_cfg.aec_limit = (uint16_t)value;
        err = nvs_set_u16(s_nvs, KEY_AEC_LIMIT, s_cfg.aec_limit);
    } else if (strcmp(key, KEY_AE_LEVEL) == 0) {
        if (value != 0xFF && value > 10) { ESP_LOGE(TAG, "ae_level 越界（0..10 或 255=不干预）：%u", (unsigned)value); return ESP_ERR_INVALID_ARG; }
        s_cfg.ae_level = (uint8_t)value;
        err = nvs_set_u8(s_nvs, KEY_AE_LEVEL, s_cfg.ae_level);
    } else if (strcmp(key, KEY_DENOISE) == 0) {
        if (value != 0xFF && value > 8) { ESP_LOGE(TAG, "denoise 越界（0..8 或 255=不干预）：%u", (unsigned)value); return ESP_ERR_INVALID_ARG; }
        s_cfg.denoise = (uint8_t)value;
        err = nvs_set_u8(s_nvs, KEY_DENOISE, s_cfg.denoise);
    } else if (strcmp(key, KEY_AE_LOCK) == 0) {
        s_cfg.ae_lock = (uint8_t)(value ? 1 : 0);
        err = nvs_set_u8(s_nvs, KEY_AE_LOCK, s_cfg.ae_lock);
    } else if (strcmp(key, KEY_WB_LOCK) == 0) {
        s_cfg.wb_lock = (uint8_t)(value ? 1 : 0);
        err = nvs_set_u8(s_nvs, KEY_WB_LOCK, s_cfg.wb_lock);
    } else if (strcmp(key, KEY_SHARPNESS) == 0) {
        if (value != 0xFF && value > 6) { ESP_LOGE(TAG, "sharpness 越界（0..6 或 255=不干预）：%u", (unsigned)value); return ESP_ERR_INVALID_ARG; }
        s_cfg.sharpness = (uint8_t)value;
        err = nvs_set_u8(s_nvs, KEY_SHARPNESS, s_cfg.sharpness);
    } else if (strcmp(key, KEY_AEC_VALUE) == 0) {
        if (value > 1200) { ESP_LOGE(TAG, "aec_value 越界（0..1200）：%u", (unsigned)value); return ESP_ERR_INVALID_ARG; }
        s_cfg.aec_value = (uint16_t)value;
        err = nvs_set_u16(s_nvs, KEY_AEC_VALUE, s_cfg.aec_value);
    } else if (strcmp(key, KEY_AGC_GAIN) == 0) {
        if (value > 30) { ESP_LOGE(TAG, "agc_gain 越界（0..30）：%u", (unsigned)value); return ESP_ERR_INVALID_ARG; }
        s_cfg.agc_gain = (uint8_t)value;
        err = nvs_set_u8(s_nvs, KEY_AGC_GAIN, s_cfg.agc_gain);
    } else if (strcmp(key, KEY_LIGHT_EN) == 0) {
        s_cfg.light_en = (uint8_t)(value ? 1 : 0);
        err = nvs_set_u8(s_nvs, KEY_LIGHT_EN, s_cfg.light_en);
        ESP_LOGW(TAG, "补光总开关 = %u（light_en 变更立即生效于下次拍照流程）", (unsigned)s_cfg.light_en);
    } else if (strcmp(key, KEY_LIGHT_MODE) == 0) {
        if (value > 1) { ESP_LOGE(TAG, "light_mode 越界（0=脉冲 / 1=常亮）：%u", (unsigned)value); return ESP_ERR_INVALID_ARG; }
        s_cfg.light_mode = (uint8_t)value;
        err = nvs_set_u8(s_nvs, KEY_LIGHT_MODE, s_cfg.light_mode);
    } else if (strcmp(key, KEY_LIGHT_LEVEL) == 0) {
        if (value > 255) { ESP_LOGE(TAG, "light_level 越界（0..255）：%u", (unsigned)value); return ESP_ERR_INVALID_ARG; }
        s_cfg.light_level = (uint8_t)value;
        err = nvs_set_u8(s_nvs, KEY_LIGHT_LEVEL, s_cfg.light_level);
    } else if (strcmp(key, KEY_LIGHT_LEAD_MS) == 0) {
        if (value > 3000) { ESP_LOGE(TAG, "light_lead_ms 越界（0..3000）：%u", (unsigned)value); return ESP_ERR_INVALID_ARG; }
        s_cfg.light_lead_ms = (uint16_t)value;
        err = nvs_set_u16(s_nvs, KEY_LIGHT_LEAD_MS, s_cfg.light_lead_ms);
    } else if (strcmp(key, KEY_LIGHT_GPIO) == 0) {
        if (value > 48) { ESP_LOGE(TAG, "light_gpio 越界（0..48）：%u", (unsigned)value); return ESP_ERR_INVALID_ARG; }
        s_cfg.light_gpio = (uint8_t)value;
        err = nvs_set_u8(s_nvs, KEY_LIGHT_GPIO, s_cfg.light_gpio);
        ESP_LOGW(TAG, "补光控制脚改为 GPIO%u（需重启/重新唤醒后生效）", (unsigned)s_cfg.light_gpio);
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
    /* 记录参数基线版本：避免下次启动触发一次无谓的"迁移重置" */
    (void)nvs_set_u8(s_nvs, KEY_CFG_VER, CFG_PARAM_VER);
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

    s_cfg.gain_ceil     = DEF_GAIN_CEIL;
    s_cfg.aec_limit     = DEF_AEC_LIMIT;
    s_cfg.ae_level      = DEF_AE_LEVEL;
    s_cfg.denoise       = DEF_DENOISE;
    s_cfg.ae_lock       = DEF_AE_LOCK;
    s_cfg.wb_lock       = DEF_WB_LOCK;
    s_cfg.sharpness     = DEF_SHARPNESS;
    s_cfg.aec_value     = DEF_AEC_VALUE;
    s_cfg.agc_gain      = DEF_AGC_GAIN;

    s_cfg.light_en      = DEF_LIGHT_EN;
    s_cfg.light_mode    = DEF_LIGHT_MODE;
    s_cfg.light_level   = DEF_LIGHT_LEVEL;
    s_cfg.light_lead_ms = DEF_LIGHT_LEAD_MS;
    s_cfg.light_gpio    = DEF_LIGHT_GPIO;

    ESP_LOGI(TAG, "配置已恢复默认");
    return ESP_OK;
}
