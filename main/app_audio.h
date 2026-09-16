/**
 * @file  app_audio.h
 * @brief 声音反馈模块（/ 4.9，REQ / REQ）
 *
 * 音频通路：I2S(MCLK=GPIO10/BCLK=11/LRCK=12/DOUT=14) → ES8311 解码 → NS4150B 功放 → 喇叭。
 * 功放使能经扩展芯片 EXIO4(PA_CTRL)：1=开 / 0=关（上电默认关断）。
 * 音效为 16 kHz / 16 bit / 单声道 PCM，内嵌 Flash（见 app_tones.h），不依赖 TF 卡。
 */
#pragma once

#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** 提示音种类（与 app_tones.h 的 g_tone_table 索引顺序一致） */
typedef enum {
    TONE_WAKEUP = 0,   /**< 上行「叮」按键唤醒/开机 0.25s */
    TONE_SHUTTER,      /**< 轻「嗒」快门 0.1s（可选） */
    TONE_SUCCESS,      /**< 双音上行「叮咚」拍照成功 0.4s */
    TONE_FAIL,         /**< 双音下行「嗡—嘟」拍照/写卡失败 0.6s */
    TONE_SD_ERROR,     /**< 三短「哔哔哔」卡缺失/容量不足 0.6s */
    TONE_LOW_BATTERY,  /**< 低频两声「咚」低电量 0.5s */
    TONE_SLEEP_WARN,   /**< 「即将休眠」双音下行 ~0.32s：会话剩余 ≤10 s 时播，播完即入睡（v1.6） */
    TONE_COUNT
} tone_id_t;

/**
 * @brief 初始化音频通路（I2S 16 kHz + ES8311 + 默认音量）
 * @note  须在 bsp_i2c_init() 与 bsp_io_expander_init() 之后调用
 * @note  失败不致命：调用方应记录告警后继续，play() 会自动降级为静默
 */
esp_err_t app_audio_init(void);

/**
 * @brief 播放提示音（阻塞到播完并关断功放）
 * @note  静音策略：audio_mute=1 时跳过常规音，仅保留失败/卡异常/低电量告警音
 */
esp_err_t app_audio_play(tone_id_t tone);

/**
 * @brief 关断功放（EXIO4=0），深睡前调用
 */
esp_err_t app_audio_power_amp_off(void);

/**
 * @brief 深睡前把音频通路整体降到最低功耗
 *
 * 动作：关功放（EXIO4=0）+ `esp_codec_dev_close()`（ES8311 mute + suspend，
 * 内部写 ES8311 低功耗寄存器序列）。
 *
 * @note 调用后本模块进入"已休眠"态，后续 play() 直接返回错误，
 *       直到重新 app_audio_init()。因此**只在真正入睡前调用**，
 *       失败保持唤醒（hold_awake_s）期间不要调用。
 */
esp_err_t app_audio_sleep(void);

#ifdef __cplusplus
}
#endif
