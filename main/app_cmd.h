/**
 * @file  app_cmd.h
 * @brief 串口命令行模块
 *
 * 前置：主控台必须是 USB-Serial-JTAG（见 sdkconfig：CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG=y）。
 *       次级 console 仅输出，无法输入/REPL（ESP-IDF 官方文档明确）。
 * 命令：help / info / status / snap / tone <id> / cfg list|set / reset / sleep
 */
#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** 手动拍照回调（由 main 提供：执行一次完整拍照流程） */
typedef void (*app_cmd_snap_fn)(void);

/** 立即入睡回调（由 main 提供：回睡音 → 资源回收/降流 → 真深睡） */
typedef void (*app_cmd_sleep_fn)(void);

/** @brief 初始化命令行（创建独立任务阻塞读取 stdin，打印 banner） */
esp_err_t app_cmd_init(void);

/** @brief 注册 snap 命令回调 */
void app_cmd_set_snap_handler(app_cmd_snap_fn fn);

/** @brief 注册 sleep 命令回调（调试用：用于深睡序列验证与深睡电流实测） */
void app_cmd_set_sleep_handler(app_cmd_sleep_fn fn);

/** @brief 由 main 更新"上次结果"，供 status 显示 */
void app_cmd_set_last_result(const char *s);

#ifdef __cplusplus
}
#endif
