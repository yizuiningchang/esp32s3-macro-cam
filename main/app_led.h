/**
 * @file  app_led.h
 * @brief USER LED 状态指示模块
 *
 * 硬件（原理图已核实）：板载 LED2（USER / PWR_LED，单色单灯）
 *   - 上电默认常亮：3V3 经 R24(3K) 直连阳极（兼电源指示）
 *   - 软件经扩展芯片 EXIO6 控制：EXIO6=0 灭 / EXIO6=1 亮；闪烁以拉低脉冲实现
 *   - 无零流态：R24/R25 分压支路恒有 ≈0.4~0.5 mA 底流（已计入）
 *
 * 语义（v1.2「工作常亮 + 告警闪烁」）：
 *   - 上电 / 唤醒 / 拍照流程中 = 常亮（默认态）
 *   - 拍照失败 / SD 告警      = 闪 3 次后恢复常亮
 *   - 拍照成功               = 不单独指示（保持常亮，由成功提示音表达）
 *   - 深睡                   = 熄灭（睡前 EXIO6=0），唤醒后恢复常亮
 *
 * @note 须在 bsp_i2c_init() 与 bsp_io_expander_init() 之后调用
 */
#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    LED_MODE_ON = 0,   /**< 常亮（工作态默认） */
    LED_MODE_OFF,      /**< 熄灭（深睡前） */
    LED_MODE_ALERT,    /**< 告警：闪 3 次后恢复常亮（拍照失败 / SD 异常） */
} led_mode_t;

/**
 * @brief 初始化并置为常亮（上电/唤醒默认工作态）
 * @note  失败不致命：调用方记录告警后继续，show() 会自动降级为静默
 */
esp_err_t app_led_init(void);

/**
 * @brief 按模式驱动 USER LED（ALERT 为阻塞式闪 3 次）
 */
esp_err_t app_led_show(led_mode_t mode);

#ifdef __cplusplus
}
#endif
