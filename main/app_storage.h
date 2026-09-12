/**
 * @file  app_storage.h
 * @brief TF 卡存储子系统
 *
 * 挂载点由 BSP 提供：CONFIG_BSP_SD_MOUNT_POINT（默认 "/sdcard"）
 * 照片路径：/sdcard/DCIM/IMG_序号.jpg
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** @brief 挂载 TF 卡（FAT32），并确保 /DCIM 目录存在 */
esp_err_t app_storage_mount(void);

/** @brief 卸载 TF 卡（深睡前必须调用） */
esp_err_t app_storage_unmount(void);

/** @brief TF 卡当前是否已挂载 */
bool app_storage_mounted(void);

/**
 * @brief 保存一帧 JPEG 为 /sdcard/DCIM/IMG_%06u.jpg
 *
 * 行为：写失败重试 2 次 → 删残件 → 返回错误；
 * 成功后调用 app_config_next_file_seq() 持久化序号，保证不覆盖。
 *
 * @param[in]  data           JPEG 数据
 * @param[in]  len            数据长度
 * @param[out] out_path       实际写入路径缓冲，可为 NULL
 * @param[in]  out_path_len   路径缓冲长度
 */
esp_err_t app_storage_save_jpeg(const uint8_t *data, size_t len, char *out_path, size_t out_path_len);

/**
 * @brief 读取容量信息
 * @param[out] total_bytes 总容量，可为 NULL
 * @param[out] free_bytes  剩余容量，可为 NULL
 */
esp_err_t app_storage_get_space(uint64_t *total_bytes, uint64_t *free_bytes);

/**
 * @brief 剩余容量是否充足
 * @param[in] low_pct 阈值百分比，默认取配置 low_space_pct
 * @return true=可继续拍摄；false=低于阈值应停拍告警
 */
bool app_storage_space_ok(uint8_t low_pct);

#ifdef __cplusplus
}
#endif
