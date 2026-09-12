/**
 * @file  main.c
 * @brief ESP32-S3 微距拍摄终端 — Phase 1 主流程
 *
 * 线性流程（深睡为常态）：
 *   按键 RTC 唤醒 → 挂载 TF 卡 → 初始化摄像头 → 对焦等待 → 抓取 JPEG
 *   → 写卡(序号命名) → 释放资源 → 声音/LED 反馈 → 回深睡
 */
#include <inttypes.h>
#include <stdbool.h>
#include <string.h>

#include "app_camera.h"
#include "app_config.h"
#include "app_power.h"
#include "app_storage.h"
#include "bsp/esp-bsp.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "main";

/** 开发期：冷启动（上电/复位）也执行一次拍照，便于调试。
 *  定版时改为 0，仅按键唤醒才拍照。 */
#define APP_CAPTURE_ON_COLD_BOOT 1

/** 开发期：流程结束后**跳过深睡**，让 USB-Serial-JTAG 保持在线，便于查看日志 /
 *  使用 IDE 监视器与烧录。
 *  原因：本机方案是"深睡为常态"，而 ESP32-S3 进入深睡会切断 USB-Serial-JTAG 供电，
 *        主机侧串口随即消失，日志窗口只剩 1~3 秒，导致无法调试。
 *  ⚠️ 定版必须为 0（��按键唤醒 → 拍照 → 回深睡）。 */
#define APP_DEBUG_KEEP_AWAKE 1

/** 单次拍照结果 */
typedef enum {
    CAP_OK = 0,
    CAP_ERR_MOUNT,     /**< TF 卡挂载失败 */
    CAP_ERR_SPACE,     /**< 容量低于阈值 */
    CAP_ERR_CAMERA,    /**< 摄像头初始化/抓帧失败 */
    CAP_ERR_WRITE,     /**< 写卡失败 */
} cap_result_t;

/**
 * @brief 单次拍照线性流程
 * @param[out] out_path 成功时回填落卡路径
 */
static cap_result_t run_capture_flow(char *out_path, size_t out_path_len)
{
    const app_cfg_t *cfg = app_config_get();
    esp_err_t err;

    /* --- 1) 挂载 TF 卡 --- */
    err = app_storage_mount();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "TF 卡不可用，终止本次拍照");
        return CAP_ERR_MOUNT;
    }

    /* --- 2) 容量检查 --- */
    if (!app_storage_space_ok(cfg->low_space_pct)) {
        ESP_LOGE(TAG, "剩余容量低于 %u%%，停拍保护历史照片", (unsigned)cfg->low_space_pct);
        return CAP_ERR_SPACE;
    }

    /* --- 3) 初始化摄像头（须在 bsp_i2c_init 之后：SCCB 复用共享 I2C） --- */
    err = app_camera_init();
    if (err != ESP_OK) {
        return CAP_ERR_CAMERA;
    }

    /* --- 4) 对焦等待 + 抓取 JPEG --- */
    camera_fb_t *fb = app_camera_capture();
    if (fb == NULL) {
        ESP_LOGE(TAG, "抓帧失败");
        return CAP_ERR_CAMERA;
    }
    ESP_LOGI(TAG, "抓帧成功：%u 字节 %ux%u %s",
             (unsigned)fb->len, (unsigned)fb->width, (unsigned)fb->height,
             fb->format == PIXFORMAT_JPEG ? "JPEG" : "非JPEG");

    /* --- 5) 写卡 --- */
    esp_err_t werr = app_storage_save_jpeg(fb->buf, fb->len, out_path, out_path_len);
    app_camera_return(fb);   /* 必须先归还帧缓冲再判断结果 */
    if (werr != ESP_OK) {
        return CAP_ERR_WRITE;
    }

    /* --- 6) 释放摄像头（用完即释放，降低功耗） --- */
    app_camera_deinit();
    return CAP_OK;
}

/**
 * @brief 结果反馈
 * TODO(audio/led)：模块就绪后替换为提示音 + 指示灯
 */
static void report_result(cap_result_t result, const char *path)
{
    switch (result) {
    case CAP_OK:
        ESP_LOGI(TAG, "拍照成功：%s", path);
        break;
    case CAP_ERR_MOUNT:
        ESP_LOGE(TAG, "[声音] 卡缺失 → 三短哔 + LED 慢闪 3 次");
        break;
    case CAP_ERR_SPACE:
        ESP_LOGE(TAG, "[声音] 容量不足 → 三短哔 + LED 慢闪 3 次");
        break;
    case CAP_ERR_CAMERA:
        ESP_LOGE(TAG, "[声音] 摄像头异常 → 下行双音 + LED 慢闪 3 次");
        break;
    case CAP_ERR_WRITE:
        ESP_LOGE(TAG, "[声音] 写卡失败 → 下行双音 + LED 慢闪 3 次");
        break;
    default:
        break;
    }
}

void app_main(void)
{
    /* --- 初始化 --- */
    ESP_ERROR_CHECK(app_config_init());
    app_power_log_wakeup_cause();

    /* 共享 I2C 总线 + 扩展 IO（摄像头 SCCB / ES8311 / CH32V003 三方共用） */
    ESP_ERROR_CHECK(bsp_i2c_init());
    ESP_ERROR_CHECK(bsp_io_expander_init());

    const bool by_button = app_power_woke_by_button();
    ESP_LOGI(TAG, "微距拍摄终端启动，唤醒源=%s", by_button ? "按键" : "非按键");

    /* --- 判定是否执行拍照 --- */
    bool do_capture = by_button;
#if APP_CAPTURE_ON_COLD_BOOT
    if (!by_button) {
        ESP_LOGW(TAG, "冷启动调试模式：强制执行一次拍照（定版请关闭 APP_CAPTURE_ON_COLD_BOOT）");
        do_capture = true;
    }
#endif

    if (do_capture) {
        char path[64] = {0};
        cap_result_t result = run_capture_flow(path, sizeof(path));
        report_result(result, path);
    }

    /* --- 回深睡（内部会释放摄像头、卸载 TF 卡、配置按键唤醒源） --- */
#if APP_DEBUG_KEEP_AWAKE
    ESP_LOGW(TAG, "调试模式：跳过深睡，保持 USB 串口在线"
                  "（定版请把 APP_DEBUG_KEEP_AWAKE 改为 0）");
    /* 即使不深睡也必须释放资源：否则 TF 卡停在挂载态，随后芯片复位
     * 可能让卡进入异常状态（后续 CMD1 全部超时，需重新插拔才恢复）。 */
    app_camera_deinit();
    app_storage_unmount();
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(5000));
        ESP_LOGI(TAG, "调试模式：设备保持唤醒中…");
    }
#else
    app_power_enter_deep_sleep();
#endif
}
