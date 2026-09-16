/**
 * @file  app_led.c
 * @brief USER LED 状态指示模块实现
 */
#include "app_led.h"

#include "bsp/esp-bsp.h"
#include "esp_check.h"
#include "esp_io_expander.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "led";

/* EXIO6 = USER LED / PWR_LED（原理图 PIN OUT 表；EXIO6=1 亮 / EXIO6=0 灭） */
#define EXIO_USER_LED  IO_EXPANDER_PIN_NUM_6

/* 告警闪烁：灭-亮交替 3 次（时长以肉眼可辨为准，阻塞执行） */
#define LED_BLINK_TIMES   3
#define LED_BLINK_OFF_MS  150
#define LED_BLINK_ON_MS   150

static esp_io_expander_handle_t s_io = NULL;

/** 电平设置：on=亮(1)、off=灭(0) */
static esp_err_t led_set(bool on)
{
    if (s_io == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    return esp_io_expander_set_level(s_io, EXIO_USER_LED, on ? 1 : 0);
}

esp_err_t app_led_init(void)
{
    s_io = bsp_get_io_expander_handle();
    if (s_io == NULL) {
        ESP_LOGE(TAG, "扩展芯片未初始化，USER LED 不可用");
        return ESP_ERR_INVALID_STATE;
    }

    /* 上电 / 唤醒 = 常亮（默认工作态） */
    ESP_RETURN_ON_ERROR(led_set(true), TAG, "点亮 USER LED 失败");
    ESP_LOGI(TAG, "USER LED 就绪（常亮）");
    return ESP_OK;
}

esp_err_t app_led_show(led_mode_t mode)
{
    if (s_io == NULL) {
        return ESP_ERR_INVALID_STATE;   /* 未初始化，静默降级 */
    }

    switch (mode) {
    case LED_MODE_ON:
        return led_set(true);

    case LED_MODE_OFF:
        return led_set(false);

    case LED_MODE_ALERT:
        /* 拉低脉冲实现「闪 3 次」，结束恢复常亮 */
        for (int i = 0; i < LED_BLINK_TIMES; i++) {
            led_set(false);
            vTaskDelay(pdMS_TO_TICKS(LED_BLINK_OFF_MS));
            led_set(true);
            vTaskDelay(pdMS_TO_TICKS(LED_BLINK_ON_MS));
        }
        ESP_LOGI(TAG, "告警：USER LED 闪 %d 次后恢复常亮", LED_BLINK_TIMES);
        return ESP_OK;

    default:
        return ESP_ERR_INVALID_ARG;
    }
}
