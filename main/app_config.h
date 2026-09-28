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
    uint8_t  frame_size;     /**< 分辨率档位（framesize_t），默认 HD 1280×720（2026-09-24 由 UXGA 降档） */
    uint16_t af_settle_ms;   /**< 对焦等待 ms，Phase 1 定焦默认 0（AF 到货后 cfg set 500） */
    uint8_t  low_space_pct;  /**< 停拍容量阈值 %，默认 5 */
    uint32_t file_seq;       /**< 文件名序号计数，默认 1 */
    uint8_t  audio_vol;      /**< 扬声器音量 0~100，默认 65 */
    uint8_t  audio_mute;     /**< 静音开关，1=静音常规提示音 */
    uint16_t hold_awake_s;   /**< 拍照失败后保持唤醒时长 s，默认 60 */

    /* --- 画质调参（v1.8：暗场竖纹治理；2026-09-24 现场工作值固化为默认）========
     * 现场改法：`cfg set <键> <值>`（写 NVS）→ `cam apply`（立即下发到传感器），无需重编。
     * 键值约定：**0xFF / 0xFFFF = 「不干预」**，保持 OV5640 寄存器表的出厂默认值。
     * ⚠ 默认值已按现场实测固化：gain_ceil=128(8x) / denoise=8 / sharpness=5(+2)，
     *   其余仍为"不干预"哨兵。**新设备（空 NVS）即获得调优画质基线**。
     *   NVS 陷阱见 DEVELOPMENT_NOTES 4.6：默认值只在键不存在时写入，旧设备不受影响。 */
    uint16_t gain_ceil;      /**< AGC 最大增益码 = 增益×16。**默认 128 = 8x（现场工作值）**；
                              *   248=出厂默认(≈15.5x)、64=4x、32=2x。
                              *   ⚠ 竖纹幅度 ∝ 模拟增益，**这是压竖纹的第一抓手**。
                              *   注意 ov5640.c 的 set_gainceiling() 是按「原始寄存器码」写的
                              *   （0x3A18=(l>>8)&3, 0x3A19=l&0xFF），不能按标准枚举传值，故本项走 set_reg。 */
    uint16_t aec_limit;      /**< AEC 曝光上限（行数，0x3A02:03）。0xFFFF=不干预（出厂 984 行=UXGA 半帧）。
                              *   出厂表把曝光锁在 984 行：UXGA(VTS=1968行)下 AE 曝光用尽后只能拉增益 → 竖纹爆。
                              *   抬到 ~1960 可让 AE「用曝光换增益」；代价是帧率最低降到 ~2fps（单张拍照无感）。
                              *   ⚠ 只对"不 binning 的大帧"有意义：4:3 binning 档(XGA/SVGA/VGA) VTS=984 行、
                              *   HD(16:9 binning) VTS=744 行，出厂 984 行上限已够甚至超额，无需调整。 */
    uint8_t  ae_level;       /**< AE 目标偏移 0..10（实为 -5..+5，**5=中性**）；0xFF=不干预。
                              *   调低→AE 目标更暗→模拟增益更低→竖纹更弱（代价：整体变暗） */
    uint8_t  denoise;        /**< OV5640 降噪档 0..8（0=关）。**默认 8（现场工作值）**；0xFF=不干预。
                              *   压短程相关噪声；暗场收益显著（亮场仅 ~3%） */
    uint8_t  ae_lock;        /**< 1=锁定曝光与增益（配 aec_value/agc_gain 使用），0=自动 */
    uint8_t  wb_lock;        /**< 1=锁定白平衡（灰度不随光源漂移，利于定标），0=自动 */
    uint16_t aec_value;      /**< 手动曝光行 0..1200（仅 ae_lock=1 时下发） */
    uint8_t  agc_gain;       /**< 手动增益档 0..30（仅 ae_lock=1 时下发） */
    uint8_t  sharpness;      /**< ISP 锐度档 0..6（实为 -3..+3，**3=中性**）。**默认 5 = +2（现场工作值）**；
                              *   0xFF=不干预。
                              *   ⚠ 只能改善"边缘对比度观感"，**不能恢复失焦丢失的细节**；
                              *   下发时会关闭 ISP 自动锐化（0x5308 bit6 清 0），切固定档。 */

    /* --- 外置补光（REQ，2026-09-19 新增）====================================
     * 控制脚走 LCD 18pin 排线座上的空闲 GPIO（无屏时可用）：
     *   pin4 = GPIO5（默认） / pin5 = GPIO1 / pin6 = GPIO2 / pin9 = GPIO6
     * 现场改法：`cfg set light_gpio <n>` 后**需重启/重新唤醒**才生效（init 时配 LEDC）。
     * 默认 light_en=0 → 不改变既有行为（Phase 1 零影响）。 */
    uint8_t  light_en;       /**< 补光总开关，0=关（默认）、1=开 */
    uint8_t  light_mode;     /**< 0=拍照脉冲（默认，省电）；1=会话常亮（AE 最稳、费电） */
    uint8_t  light_level;    /**< 亮度/PWM 占空比 0~255，默认 255（抓帧建议满亮，避免波纹进画面） */
    uint16_t light_lead_ms;  /**< 点亮 → 抓帧 的等待 ms（等 AE 收敛），默认 200 */
    uint8_t  light_gpio;     /**< 控制脚 GPIO 号，默认 5（= LCD 座 pin4） */
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
 *             / "gain_ceil" / "aec_limit" / "ae_level" / "denoise" / "ae_lock"
 *             / "wb_lock" / "aec_value" / "agc_gain"
 *             / "light_en" / "light_mode" / "light_level" / "light_lead_ms" / "light_gpio"
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
