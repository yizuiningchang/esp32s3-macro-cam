/**
 * @file  app_power.h
 * @brief 电源与低功耗模块
 *
 * 拍照键 = **PWR = GPIO15**（RTC GPIO，低有效，板上有 R27 10K 外部上拉）
 *
 * ⚠ 为什么**不用** BOOT/GPIO0（2026-09-13 实机教训，务必保留此说明）：
 *   GPIO0 是 ESP32-S3 的 **strapping 引脚**，而**深睡唤醒本质就是一次芯片复位**——
 *   唤醒你的那次按键正好把 GPIO0 按住为低，ROM 在复位瞬间采样到 0，会判定为
 *   "下载模式"，**应用直接不启动**（现象：按了没声音、没照片、串口无应用输出），
 *   而且可能卡住（ESP-IDF issue IDFGH-13141：仅断电重启未必恢复，需 EN 复位）。
 *   GPIO15 非 strapping，且属 RTC GPIO（ESP32-S3: 0~21），可作 ext1 唤醒源。
 *
 * 深睡唤醒仅保留按键一路：ext1 + ANY_LOW
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** 拍照键引脚（PWR 按键；原理图 PWR_KEY → IO15，R27 10K 上拉，低有效） */
#define APP_BUTTON_PIN 15

/** @brief 按键引脚（普通 GPIO 模式，用于读电平/消抖）初始化 */
esp_err_t app_power_button_init(void);

/**
 * @brief 配置深睡唤醒源（ext1 / PWR:IO15 / 低电平）
 * @note  ESP32-S3 只支持 ANY_LOW（任一选中引脚为低即唤醒）；
 *        ESP_EXT1_WAKEUP_ALL_LOW 仅 ESP32 支持，本芯片不可用。
 */
esp_err_t app_power_prepare_button_wake(void);

/** @brief 唤醒原因是否为按键（GPIO/ext1） */
bool app_power_woke_by_button(void);

/** @brief 原始读拍照键电平（0=按下 / 1=松开；不消抖，自检/调试用） */
int app_power_button_level(void);

/** @brief 按键是否处于按下状态（含 50 ms 软件消抖） */
bool app_power_button_pressed(void);

/** @brief 阻塞等待按键释放，用于深睡前防误唤醒 */
void app_power_wait_button_release(uint32_t timeout_ms);

/**
 * @brief 等待按键释放，并判定是否"长按"
 * @param long_ms 长按阈值（ms）
 * @return true = 按住时长达标（长按，可能仍未松手）；false = 阈值内已松手（短按）
 * @note 会话内用于区分：短按 = 拍照；长按 = 播「即将休眠」后立即休眠
 */
bool app_power_wait_button_release_long(uint32_t long_ms);

/** @brief 打印本次复位/唤醒原因 */
void app_power_log_wakeup_cause(void);

/**
 * @brief 深睡前资源回收 + 板级外设降流
 *
 * 顺序：释放摄像头驱动 → 断言摄像头 PWDN(EXIO3) → 卸载 TF 卡 →
 *       关功放(EXIO4=0) → ES8311 低功耗 → 熄灭 USER LED(EXIO6=0) → 刷日志。
 *
 * @note 幂等；**只在真正入睡前调用**——失败保持唤醒（hold_awake_s）期间不要调用，
 *       因为其中的 `app_audio_sleep()` 会关掉 codec，之后无法再播报提示音。
 */
esp_err_t app_power_prepare_sleep(void);

/**
 * @brief 释放外设资源并进入深睡（不返回）
 */
void app_power_enter_deep_sleep(void);

/** @brief 读电池 ADC 原始值（经扩展芯片 BAT_ADC，未标定前仅供记录/标定） */
uint16_t app_power_battery_adc(void);

/**
 * @brief 电量是否"极低"
 *
 * 要求失败兜底入睡需"电量非极低"，但电池分压比与 ADC 量程**尚未标定**，
 * 故判定阈值以 `APP_BAT_LOW_ADC` 宏表示，默认 0 = 判定禁用（只记录原始 ADC）。
 * 标定后改该宏并把实测值补进 DEVELOPMENT_NOTES。
 */
bool app_power_battery_low(void);

#ifdef __cplusplus
}
#endif
