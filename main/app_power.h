/**
 * @file  app_power.h
 * @brief 电源与低功耗模块
 *
 * 拍照键 = BOOT = GPIO0（RTC GPIO，低有效）
 * 深睡唤醒仅保留按键一路：ext1 + ANY_LOW
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** 拍照键引脚（BOOT 按键） */
#define APP_BUTTON_PIN 0

/** @brief 按键引脚（普通 GPIO 模式，用于读电平/消抖）初始化 */
esp_err_t app_power_button_init(void);

/**
 * @brief 配置深睡唤醒源（ext1 / GPIO0 / 低电平）
 * @note  ESP32-S3 只支持 ANY_LOW（任一选中引脚为低即唤醒）；
 *        ESP_EXT1_WAKEUP_ALL_LOW 仅 ESP32 支持，本芯片不可用。
 */
esp_err_t app_power_prepare_button_wake(void);

/** @brief 唤醒原因是否为按键（GPIO/ext1） */
bool app_power_woke_by_button(void);

/** @brief 按键是否处于按下状态（含 50 ms 软件消抖） */
bool app_power_button_pressed(void);

/** @brief 阻塞等待按键释放，用于深睡前防误唤醒 */
void app_power_wait_button_release(uint32_t timeout_ms);

/** @brief 打印本次复位/唤醒原因（日志可追溯，异常#7） */
void app_power_log_wakeup_cause(void);

/**
 * @brief 释放外设资源并进入深睡（不返回）
 *
 * 顺序：释放摄像头 → 卸载 TF 卡 → 关功放(EXIO4=0) → 刷日志 → 进深睡
 */
void app_power_enter_deep_sleep(void);

#ifdef __cplusplus
}
#endif
