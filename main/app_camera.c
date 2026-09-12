/**
 * @file  app_camera.c
 * @brief 摄像头子系统实现
 */
#include "app_camera.h"

#include "app_config.h"
#include "bsp/esp-bsp.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "camera";

static bool s_inited = false;

esp_err_t app_camera_init(void)
{
    if (s_inited) {
        return ESP_OK;
    }

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
    return ESP_OK;
}

camera_fb_t *app_camera_capture(void)
{
    if (!s_inited) {
        ESP_LOGE(TAG, "摄像头未初始化");
        return NULL;
    }

    /* AF 模组到货后此处触发对焦并等待 cfg->af_settle_ms；Phase 1 定焦模组固定等待 */
    const app_cfg_t *cfg = app_config_get();
    if (cfg->af_settle_ms > 0) {
        vTaskDelay(pdMS_TO_TICKS(cfg->af_settle_ms));
    }

    /* 丢弃首帧：生效最新的曝光/白平衡参数 */
    camera_fb_t *fb = esp_camera_fb_get();
    if (fb) {
        esp_camera_fb_return(fb);
    }

    fb = esp_camera_fb_get();
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

    /* TODO(EXIO3)：按原理图确认扩展 IO 位后，此处拉 PWDN 进一步降低深睡电流。
     * 注意：的 EXIO 映射（2=SD CS / 3=CAM PWDN / 4=功放 / 6=LED）
     * 与 BSP 头文件注释（1=LCD 背光 / 2=LCD RST / 4=功放 / 6=LED）存在冲突，
     * 未核对原理图前不要写入具体引脚。 */
    ESP_LOGI(TAG, "摄像头已释放：%s", esp_err_to_name(err));
    return err;
}
