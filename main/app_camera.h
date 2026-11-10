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
 * 官方 BSP 从未驱动该脚，故 EXIO3 极性为"待实测"，
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

/**
 * @brief 把 NVS 画质调参下发到 OV5640（v1.8：暗场竖纹治理）
 *
 * 读取 app_config 里的 gain_ceil / ae_level / denoise / ae_lock / wb_lock /
 * aec_value / agc_gain 并逐项下发。约定 **0xFF / 0xFFFF = 不干预**（跳过该项）。
 *
 * - `app_camera_init()` 成功后会自动调用一次，因此 NVS 里的参数掉电后仍然生效。
 * - 现场改完 `cfg set <键> <值>` 后可再执行 `cam apply` 立即重下发，无需重编。
 *
 * @note 摄像头未初始化时返回 ESP_ERR_INVALID_STATE。
 */
esp_err_t app_camera_apply_tuning(void);

/**
 * @brief 打印当前生效的画质参数（从传感器回读，而非从配置读）
 *
 * 用于确认下发是否真的落到寄存器上。同时回读 AE 落点（曝光行 / 增益码），
 * 这是判断"竖纹是否由模拟增益拉满引起"的直接证据。
 */
void app_camera_dump_tuning(void);

/**
 * @brief 打一行本帧 AE 落点日志（曝光行数 / 增益倍数，v1.9 竖纹归因取证）
 *
 * 拍照流程抓帧成功后调用：竖纹帧的增益应显著高于干净帧，
 * 把该值与落卡照片一一配对，即可长期归因画质问题（照片本身无 EXIF）。
 */
void app_camera_log_ae(void);

#ifdef __cplusplus
}
#endif
