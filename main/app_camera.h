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
 * @note 前置条件：bsp_i2c_init() 已调用（SCCB 复用共享 I2C 总线）
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
 * @brief 释放摄像头（深睡前必须调用，配合 EXIO3 拉 PWDN 降电流）
 */
esp_err_t app_camera_deinit(void);

#ifdef __cplusplus
}
#endif
