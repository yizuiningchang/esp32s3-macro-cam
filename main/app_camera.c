/**
 * @file  app_camera.c
 * @brief 摄像头子系统实现
 */
#include "app_camera.h"

#include "app_config.h"
#include "bsp/esp-bsp.h"
#include "esp_io_expander.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "camera";

/* EXIO3 = 摄像头 PWDN（原理图 PIN OUT 表；��。
 * 官方 BSP 只把 EXIO0/1/2/4/5/6 配为输出，EXIO3 从未被驱动，故其有效电平
 * 在文档中标注为"待实测"。约定 OV5640 PWDN 高有效（=1 关断摄像头）。
 * 阶段 4 实测若深睡电流不降（或摄像头行为异常），把 CAM_PWDN_ACTIVE_LEVEL 改 0 重测。 */
#define EXIO_CAM_PWDN           IO_EXPANDER_PIN_NUM_3
#define CAM_PWDN_ACTIVE_LEVEL   1
#define CAM_PWDN_INACTIVE_LEVEL (1 - CAM_PWDN_ACTIVE_LEVEL)

/* 抓帧前稳定延时：等 XCLK/PLL 稳定 + 大负载错峰 */
#define CAPTURE_SETTLE_MS       15

static bool s_inited = false;
static bool s_first_capture_after_init = true;  /**< init 后首张需丢帧（AE/AWB 未收敛） */

esp_err_t app_camera_power_up(void)
{
    esp_io_expander_handle_t io = bsp_get_io_expander_handle();
    if (io == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    esp_err_t err = esp_io_expander_set_dir(io, EXIO_CAM_PWDN, IO_EXPANDER_OUTPUT);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "设置 EXIO3(PWDN) 方向失败：%s", esp_err_to_name(err));
        return err;
    }
    return esp_io_expander_set_level(io, EXIO_CAM_PWDN, CAM_PWDN_INACTIVE_LEVEL);
}

esp_err_t app_camera_power_down(void)
{
    esp_io_expander_handle_t io = bsp_get_io_expander_handle();
    if (io == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    esp_err_t err = esp_io_expander_set_dir(io, EXIO_CAM_PWDN, IO_EXPANDER_OUTPUT);
    if (err == ESP_OK) {
        err = esp_io_expander_set_level(io, EXIO_CAM_PWDN, CAM_PWDN_ACTIVE_LEVEL);
    }
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "摄像头 PWDN 已断言（EXIO3=%d）", (int)CAM_PWDN_ACTIVE_LEVEL);
    } else {
        ESP_LOGW(TAG, "断言 PWDN 失败：%s", esp_err_to_name(err));
    }
    return err;
}

esp_err_t app_camera_init(void)
{
    if (s_inited) {
        return ESP_OK;
    }

    /* 深睡唤醒后扩展芯片仍保持 PWDN 断言态，必须先解除，否则摄像头不可用 */
    app_camera_power_up();

    const app_cfg_t *cfg = app_config_get();

    /* 以 BSP 默认配置为基底（引脚、XCLK=20MHz、SCCB 走共享 I2C） */
    camera_config_t cam_cfg = BSP_CAMERA_DEFAULT_CONFIG;

    /* 本项目需要的三项覆盖：JPEG 输出 + UXGA + 配置的 JPEG 质量 */
    cam_cfg.pixel_format = PIXFORMAT_JPEG;
    cam_cfg.frame_size   = (framesize_t)cfg->frame_size;   /* 默认 FRAMESIZE_UXGA */
    cam_cfg.jpeg_quality = cfg->jpeg_quality;              /* 默认 12 */
    cam_cfg.fb_count     = 1;                              /* 单帧抓取，省 PSRAM */
    cam_cfg.fb_location  = CAMERA_FB_IN_PSRAM;
    cam_cfg.grab_mode    = CAMERA_GRAB_WHEN_EMPTY;

    esp_err_t err = esp_camera_init(&cam_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "摄像头初始化失败（JPEG/UXGA）：%s", esp_err_to_name(err));

        /* 降级重试：QVGA/RGB565，用于快速判断是配置问题还是硬件问题 */
        esp_camera_deinit();
        cam_cfg.frame_size   = FRAMESIZE_QVGA;
        cam_cfg.pixel_format = PIXFORMAT_RGB565;
        err = esp_camera_init(&cam_cfg);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "摄像头降级初始化仍失败：%s", esp_err_to_name(err));
            return err;
        }
        ESP_LOGW(TAG, "摄像头已降级为 QVGA/RGB565 运行");
    } else {
        sensor_t *s = esp_camera_sensor_get();
        ESP_LOGI(TAG, "摄像头就绪：PID=0x%04X 分辨率=%d 质量=%u",
                 s ? s->id.PID : 0, (int)cam_cfg.frame_size, (unsigned)cfg->jpeg_quality);
    }

    s_inited = true;
    s_first_capture_after_init = true;   /* 每次重新 init 后，首张恢复丢帧收敛 */
    return ESP_OK;
}

camera_fb_t *app_camera_capture(void)
{
    if (!s_inited) {
        ESP_LOGE(TAG, "摄像头未初始化");
        return NULL;
    }

    /* AF 模组到货后此处触发对焦并等待 cfg->af_settle_ms；Phase 1 定焦模组不等待 */
    const app_cfg_t *cfg = app_config_get();
    if (cfg->af_settle_ms > 0) {
        vTaskDelay(pdMS_TO_TICKS(cfg->af_settle_ms));
    }

    /* 抓帧前稳定延时：等 XCLK/PLL 稳，并与其它大负载错峰 */
    vTaskDelay(pdMS_TO_TICKS(CAPTURE_SETTLE_MS));

    /* 丢首帧策略（v1.7）：仅**本次 init 后的第一张**丢弃——AE/AWB 尚未收敛。
     * 后续张不再单独丢帧：快门音（~370 ms 阻塞播放）期间摄像头 DMA 持续出流，
     * 已等效完成"等待最新参数生效"，单独丢帧反而白等一帧（~200 ms）。
     * （快门音在 main.c 的 run_capture_flow 中、调用本函数之前播放。） */
    if (s_first_capture_after_init) {
        s_first_capture_after_init = false;
        camera_fb_t *stale = esp_camera_fb_get();
        if (stale) {
            esp_camera_fb_return(stale);
        }
    }

    camera_fb_t *fb = esp_camera_fb_get();
    if (fb == NULL) {
        ESP_LOGE(TAG, "抓帧失败");
    }
    return fb;
}

void app_camera_return(camera_fb_t *fb)
{
    if (fb) {
        esp_camera_fb_return(fb);
    }
}

esp_err_t app_camera_deinit(void)
{
    if (!s_inited) {
        return ESP_OK;
    }
    esp_err_t err = esp_camera_deinit();
    s_inited = false;

    /* 深睡前的 PWDN 断言由 app_power_prepare_sleep() 显式调用 app_camera_power_down()，
     * 见/ 4.2（EXIO 映射以原理图 PIN OUT 表为准：EXIO3 = CAM PWDN）。 */
    ESP_LOGI(TAG, "摄像头已释放：%s", esp_err_to_name(err));
    return err;
}
