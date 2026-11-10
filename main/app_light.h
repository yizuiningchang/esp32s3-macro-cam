/**
 * @file  app_light.h
 * @brief 外置补光（LED 灯板）控制模块
 *
 * 需求：拍照时自动点亮补光、拍完自动熄灭，用于 10 cm 微距拍细节时压低噪声
 *      （暗场竖纹幅度 ∝ 模拟增益，补光是治本手段）。
 *
 * 硬件通路（原理图核查 2026-09-19）：
 *   控制信号：LCD 18pin 排线座 L3 的空闲引脚（无 LCD 时可用）
 *             pin4 = GPIO5（默认，原理图 LCD_SCK） / 可选 pin5 = GPIO1
 *   供电    ：pin1 = 3V3、pin3 = GND（小电流）；大电流请从电池座 J4 取 VBAT
 *   驱动    ：GPIO → MOS（低边开关）→ 灯板；**必须** 在 MOS 栅源加 100K 下拉
 *   ⚠ 绝不可用 I2C 座子（J6，共享总线）或 UART 座子（J11，= TF 卡 CMD/D0）驱动灯。
 *
 * 实现要点：
 *   - 用 LEDC 硬件 PWM（ESP32-S3 仅低速组），20 kHz、10 bit → 可调光且波纹不进画面；
 *   - 亮度 0~255 映射到占空比 0~1023；0 = 灭；
 *   - 深睡前把脚固定在低电平并 hold，防止 MOS 栅极悬空导致灯微亮（漏进续航）；
 *   - 默认 NVS `light_en = 0`（不点亮）→ 不改变 Phase 1 既有行为。
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 初始化补光通路（配置 LEDC 通道与 GPIO，输出默认为灭）
 *
 * 无论 `light_en` 是否为 0 都会配置好引脚（便于串口 `light on` 现场试灯）。
 * 失败只返回错误码，不影响拍照流程。
 */
esp_err_t app_light_init(void);

/** @brief 补光是否已初始化可用 */
bool app_light_ready(void);

/** @brief 补光总开关（NVS light_en） */
bool app_light_enabled(void);

/**
 * @brief 设置亮度（0~255）并立即生效（**不写 NVS**，供现场试灯）
 * @param level 0 = 灭；255 = 满亮
 */
esp_err_t app_light_set_level(uint8_t level);

/** @brief 取当前生效亮度（0~255） */
uint8_t app_light_level(void);

/** @brief 按 NVS `light_level` 点亮 */
esp_err_t app_light_on(void);

/** @brief 熄灭 */
esp_err_t app_light_off(void);

/**
 * @brief 试灯：点亮 `on_ms` 毫秒后自动熄灭（阻塞，串口命令用）
 */
esp_err_t app_light_pulse(uint16_t on_ms);

/**
 * @brief 拍照前调用：按 `light_en`/`light_mode` 决定是否点亮 + 等 AE 收敛
 *
 * 脉冲模式（light_mode=0）：点亮 → 延时 `light_lead_ms` → 返回（随后抓帧）
 * 会话常亮模式（light_mode=1）：确保常亮即可（AE 已稳定，不额外等待）
 */
void app_light_before_capture(void);

/**
 * @brief 拍照后调用：脉冲模式下熄灭；会话常亮模式保持
 */
void app_light_after_capture(void);

/**
 * @brief 深睡前调用：把控制脚固定在低电平并 hold（防灯微亮 / 防栅极悬空）
 */
void app_light_prepare_sleep(void);

/**
 * @brief 自检：把亮度写到**扩展芯片的 PWM 通道**（EXIO_PWM1 = LCD_BL 脚）
 *
 * 用途：验证「CH32V003 出厂固件是否真的支持 I2C PWM 命令、且 PWM 落在 LCD_BL 脚上」。
 * 与主通路（LEDC/GPIO5）互不影响，仅作 A/B 对照实验。
 * @param value 0~255（0 = 关）
 */
esp_err_t app_light_expander_pwm(uint8_t value);

#ifdef __cplusplus
}
#endif
