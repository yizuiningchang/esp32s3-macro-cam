/**
 * @file  app_power.c
 * @brief 电源与低功耗模块实现
 */
#include "app_power.h"

#include "app_camera.h"
#include "app_storage.h"
#include "driver/gpio.h"
#include "driver/rtc_io.h"
#include "esp_log.h"
#include "esp_sleep.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "power";

#define DEBOUNCE_MS 50

esp_err_t app_power_button_init(void)
{
    gpio_config_t io = {
        .pin_bit_mask = 1ULL << APP_BUTTON_PIN,
        .mode         = GPIO_MODE_INPUT,
        .pull_up_en   = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    return gpio_config(&io);
}

esp_err_t app_power_prepare_button_wake(void)
{
    /* 拍照键为低有效。GPIO0 是 strapping 引脚，板上已有外部上拉，
     * 故此处不启用内部上拉，避免深睡时 RTC 上拉电阻带来额外电流。 */
    rtc_gpio_init(APP_BUTTON_PIN);
    rtc_gpio_set_direction(APP_BUTTON_PIN, RTC_GPIO_MODE_INPUT_ONLY);
    rtc_gpio_pullup_dis(APP_BUTTON_PIN);
    rtc_gpio_pulldown_dis(APP_BUTTON_PIN);

    /* ESP32-S3：低电平唤醒必须用 ANY_LOW。
     * 原写 ESP_EXT1_WAKEUP_ALL_LOW，该枚举仅 ESP32 适用，需更正。 */
    esp_err_t err = esp_sleep_enable_ext1_wakeup_io(1ULL << APP_BUTTON_PIN, ESP_EXT1_WAKEUP_ANY_LOW);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "配置 ext1 唤醒失败：%s", esp_err_to_name(err));
    }
    return err;
}

bool app_power_woke_by_button(void)
{
    esp_sleep_wakeup_cause_t cause = esp_sleep_get_wakeup_cause();
    return (cause == ESP_SLEEP_WAKEUP_EXT1) || (cause == ESP_SLEEP_WAKEUP_GPIO);
}

void app_power_log_wakeup_cause(void)
{
    esp_sleep_wakeup_cause_t cause = esp_sleep_get_wakeup_cause();
    const char *name = "未知";
    switch (cause) {
    case ESP_SLEEP_WAKEUP_UNDEFINED: name = "上电/复位"; break;
    case ESP_SLEEP_WAKEUP_EXT1:      name = "按键(ext1)"; break;
    case ESP_SLEEP_WAKEUP_TIMER:     name = "定时器"; break;
    case ESP_SLEEP_WAKEUP_GPIO:      name = "GPIO"; break;
    default: break;
    }
    ESP_LOGI(TAG, "唤醒原因：%s (code=%d)", name, (int)cause);
}

bool app_power_button_pressed(void)
{
    if (gpio_get_level(APP_BUTTON_PIN) != 0) {
        return false;   /* 高电平 = 未按下 */
    }
    vTaskDelay(pdMS_TO_TICKS(DEBOUNCE_MS));
    return gpio_get_level(APP_BUTTON_PIN) == 0;
}

void app_power_wait_button_release(uint32_t timeout_ms)
{
    TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(timeout_ms);
    while (gpio_get_level(APP_BUTTON_PIN) == 0) {
        if (xTaskGetTickCount() > deadline) {
            ESP_LOGW(TAG, "等待按键释放超时");
            return;
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

void app_power_enter_deep_sleep(void)
{
    ESP_LOGI(TAG, "准备进入深睡");

    /* 1) 释放摄像头（后续可配合扩展 IO 拉 PWDN 进一步降流） */
    app_camera_deinit();

    /* 2) 卸载 TF 卡 */
    app_storage_unmount();

    /* 3) TODO(audio)：确认 EXIO 位后，此处关断功放（EXIO4=0）并让 ES8311 进低功耗 */

    /* 4) TODO(led)：确认 EXIO6 极性后，此处熄灭 USER LED */

    /* 5) 配置按键唤醒源 */
    app_power_prepare_button_wake();

    /* 6) 配置唤醒源必须在进睡眠前、且需保证按键已松开，避免被同一次按键反复唤醒 */
    app_power_button_init();
    app_power_wait_button_release(1000);

    ESP_LOGI(TAG, "进入深睡，等待按键唤醒");
    esp_deep_sleep_start();
}
