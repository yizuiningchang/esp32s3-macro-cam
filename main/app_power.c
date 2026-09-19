/**
 * @file  app_power.c
 * @brief 电源与低功耗模块实现
 */
#include "app_power.h"

#include "app_audio.h"
#include "app_camera.h"
#include "app_led.h"
#include "app_light.h"
#include "app_storage.h"
#include "bsp/esp-bsp.h"
#include "driver/gpio.h"
#include "driver/rtc_io.h"
#include "esp_log.h"
#include "esp_sleep.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "power";

#define DEBOUNCE_MS 50

/** 入睡前的刷日志延时（ms）：USB-Serial-JTAG 的 TX FIFO 是**异步排空**的，
 *  若打完最后一行日志立刻断电进深睡，FIFO 里未发出的字节会丢（表现为"最后一条日志看不到"）。
 *  等这段时间让 FIFO 排空再睡；对续航影响可忽略（一次性 ~0.2 s）。 */
#define SLEEP_LOG_FLUSH_MS 200

/* 电量“极低”阈值（扩展芯片 BAT_ADC 原始值）。0 = 未标定 → 禁用判定，仅记录。
 * 标定方法：用可调电源/满电与低电电池各测一次 BAT_ADC，取"需告警电压"对应值。 */
#define APP_BAT_LOW_ADC  0

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
    /* 拍照键（PWR/GPIO15）为低有效。板上已有 R27 10K 外部上拉，
     * 故此处不启用内部上拉，避免深睡时 RTC 上拉电阻带来额外电流。
     * ⚠ 切勿改回 GPIO0：GPIO0 是 strapping 引脚，深睡唤醒即复位，按键拉低会让
     *   ROM 进下载模式导致应用不启动（详见 app_power.h 顶部说明）。 */
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

    /* 复位原因（区分上电/深睡唤醒/软件复位/brownout 等，用于诊断异常重启） */
    const char *rname = "未知";
    switch (esp_reset_reason()) {
    case ESP_RST_POWERON:    rname = "上电"; break;
    case ESP_RST_EXT:        rname = "外部/RST引脚"; break;
    case ESP_RST_SW:         rname = "软件复位"; break;
    case ESP_RST_PANIC:      rname = "异常/Panic"; break;
    case ESP_RST_INT_WDT:    rname = "中断看门狗"; break;
    case ESP_RST_TASK_WDT:   rname = "任务看门狗"; break;
    case ESP_RST_WDT:        rname = "其他看门狗"; break;
    case ESP_RST_DEEPSLEEP:  rname = "深睡唤醒"; break;
    case ESP_RST_BROWNOUT:   rname = "BROWNOUT(掉电)"; break;
    case ESP_RST_SDIO:       rname = "SDIO"; break;
    case ESP_RST_USB:        rname = "USB复位"; break;
    case ESP_RST_JTAG:       rname = "JTAG复位"; break;
    case ESP_RST_EFUSE:      rname = "eFuse复位"; break;
    case ESP_RST_PWR_GLITCH: rname = "电源毛刺"; break;
    case ESP_RST_CPU_LOCKUP: rname = "CPU锁死"; break;
    default: break;
    }
    ESP_LOGI(TAG, "复位原因：%s (code=%d)", rname, (int)esp_reset_reason());
}

int app_power_button_level(void)
{
    return gpio_get_level(APP_BUTTON_PIN);
}

bool app_power_button_pressed(void)
{
    if (app_power_button_level() != 0) {
        return false;   /* 高电平 = 未按下 */
    }
    vTaskDelay(pdMS_TO_TICKS(DEBOUNCE_MS));
    return app_power_button_level() == 0;
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

bool app_power_wait_button_release_long(uint32_t long_ms)
{
    const TickType_t t0 = xTaskGetTickCount();
    while (gpio_get_level(APP_BUTTON_PIN) == 0) {
        if ((xTaskGetTickCount() - t0) >= pdMS_TO_TICKS(long_ms)) {
            return true;    /* 已达长按阈值（用户可能仍按着，直接按长按处理） */
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    return false;           /* 阈值内已松手 → 短按 */
}

uint16_t app_power_battery_adc(void)
{
    return bsp_get_io_expander_adc();
}

bool app_power_battery_low(void)
{
#if (APP_BAT_LOW_ADC > 0)
    uint16_t adc = app_power_battery_adc();
    return (adc > 0) && (adc < APP_BAT_LOW_ADC);
#else
    return false;   /* 未标定：不做判定（只记原始 ADC） */
#endif
}

esp_err_t app_power_prepare_sleep(void)
{
    ESP_LOGI(TAG, "开始睡眠前资源回收与降流");

    /* 1) 释放摄像头驱动 */
    app_camera_deinit();

    /* 2) 断言摄像头 PWDN（扩展芯片 EXIO3）——官方 BSP 从未驱动该脚，极性待实测确认 */
    app_camera_power_down();

    /* 3) 卸载 TF 卡：否则卡停在挂载态，芯片复位后可能卡死（须断电恢复） */
    app_storage_unmount();

    /* 4) 关断功放（EXIO4=0） */
    app_audio_power_amp_off();

    /* 5) ES8311 进低功耗（codec close → mute + suspend，写低功耗寄存器序列） */
    app_audio_sleep();

    /* 6) 熄灭 USER LED（EXIO6=0，睡眠=灭；v1.2 §4.9） */
    app_led_show(LED_MODE_OFF);

    /* 7) 补光：占空比清零 + 控制脚固定低电平并 hold（防深睡期间栅极悬空导致灯微亮漏电） */
    app_light_prepare_sleep();

    ESP_LOGI(TAG, "降流完成：PWDN 断言 / TF 卸载 / 功放关 / ES8311 低功耗 / LED 灭 / 补光脚 hold 低");
    return ESP_OK;
}

void app_power_enter_deep_sleep(void)
{
    ESP_LOGI(TAG, "准备进入深睡（电量 ADC=%u）", (unsigned)app_power_battery_adc());

    app_power_prepare_sleep();

    /* 1) 先用数字读确认按键已松开，避免被同一次按键立刻反复唤醒 */
    app_power_wait_button_release(1000);

    /* 2) 最后再切到 RTC 唤醒配置（ext1 / PWR:IO15 / 低电平）。此后不再改动该脚数字配置，
     *    以免把 RTC 域的 ext1 配置重置掉。 */
    app_power_prepare_button_wake();

    ESP_LOGI(TAG, "进入深睡，等待按键唤醒（ext1 / PWR:IO%d / 低电平）", APP_BUTTON_PIN);

    /* 3) 刷日志延时：让 USB-Serial-JTAG 的 TX FIFO 排空，避免丢掉最后几行日志 */
    vTaskDelay(pdMS_TO_TICKS(SLEEP_LOG_FLUSH_MS));

    esp_deep_sleep_start();
}
