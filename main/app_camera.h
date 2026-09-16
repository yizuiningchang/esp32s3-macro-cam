/**
 * @file  app_camera.h
 * @brief 摄像头子系统
 *
 * 引脚由微雪 BSP 的 BSP_CAMERA_DEFAULT_CONFIG 提供（与原理图一致）：
 *   XCLK=38 PCLK=41 VSYNC=17 HREF=18 D0~D7=45/47/48/46/42/40/39/21
 *   SCCB 不占独立引脚，走共享 I2C 总线 SCL=7 / SDA=8（故须先 bsp_i2c_init）
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"
#include "esp_camera.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 初始化摄像头（JPEG 输出、UXGA、帧缓冲放 PSRAM）
 *
 * @note 前置条件：bsp_i2c_init() 与 bsp_io_expander_init() 已调用
 *       （SCCB 复用共享 I2C 总线；PWDN 走扩展芯片 EXIO3）
 *       失败时会自动降级重试一次 QVGA/RGB565（对齐官方示例行为）
 */
esp_err_t app_camera_init(void);

/**
 * @brief 抓取一帧 JPEG
 * @return 帧缓冲指针，失败返回 NULL；用完必须调用 app_camera_return()
 */
camera_fb_t *app_camera_capture(void);

/** @brief 归还帧缓冲 */
void app_camera_return(camera_fb_t *fb);

/**
 * @brief 释放摄像头（深睡前必须调用，随后配合 app_camera_power_down() 降电流）
 */
esp_err_t app_camera_deinit(void);

/**
 * @brief 断言摄像头 PWDN（扩展芯片 EXIO3），深睡前调用以进一步降流
 *
 * / 4.2：官方 BSP 从未驱动该脚，故 EXIO3 极性为"待实测"，
 * 需以阶段 4 的深睡电流实测结果确认（宏见 app_camera.c）。
 */
esp_err_t app_camera_power_down(void);

/**
 * @brief 解除 PWDN（唤醒后、初始化前调用）
 *
 * @note 深睡唤醒时扩展芯片保持上次输出，PWDN 仍处断言态，
 *       因此 app_camera_init() 内部会先调用本函数，否则摄像头无法初始化。
 */
esp_err_t app_camera_power_up(void);

#ifdef __cplusplus
}
#endif
