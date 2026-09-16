/**
 * @file  app_config.h
 * @brief NVS 参数配置模块
 *
 * NVS 命名空间：crackcam
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** 运行期配置快照（启动时从 NVS 载入） */
typedef struct {
    uint8_t  jpeg_quality;   /**< JPEG 质量 0~63，越小越好，默认 12 */
    uint8_t  frame_size;     /**< 分辨率档位（framesize_t），默认 UXGA */
    uint16_t af_settle_ms;   /**< 对焦等待 ms，Phase 1 定焦默认 0（AF 到货后 cfg set 500） */
    uint8_t  low_space_pct;  /**< 停拍容量阈值 %，默认 5 */
    uint32_t file_seq;       /**< 文件名序号计数，默认 1 */
    uint8_t  audio_vol;      /**< 扬声器音量 0~100，默认 65 */
    uint8_t  audio_mute;     /**< 静音开关，1=静音常规提示音 */
    uint16_t hold_awake_s;   /**< 拍照失败后保持唤醒时长 s，默认 60 */
} app_cfg_t;

/**
 * @brief 初始化 NVS 并载入配置（缺失项写入默认值）
 */
esp_err_t app_config_init(void);

/** @brief 获取当前配置只读指针 */
const app_cfg_t *app_config_get(void);

/**
 * @brief 按 key 修改并持久化配置项
 * @param key  "jpeg_quality" / "frame_size" / "af_settle_ms" / "low_space_pct"
 *             / "audio_vol" / "audio_mute" / "hold_awake_s"
 */
esp_err_t app_config_set(const char *key, uint32_t value);

/**
 * @brief 取用并自增文件序号（写 NVS 持久化，保证断电重启不重复）
 * @param[out] out 本次应使用的序号
 */
esp_err_t app_config_next_file_seq(uint32_t *out);

/**
 * @brief 直接把文件序号对齐到指定值并持久化
 *
 * 用途：挂卡后发现卡上已有 IMG_xxx.jpg（例如 NVS 被清过、或换卡），
 * 把计数器抬到"卡上最大序号+1"，避免覆盖历史照片（REQ）。
 */
esp_err_t app_config_set_file_seq(uint32_t seq);

/** @brief 恢复出厂默认（清空命名空间后重新写入默认值） */
esp_err_t app_config_reset(void);

#ifdef __cplusplus
}
#endif
