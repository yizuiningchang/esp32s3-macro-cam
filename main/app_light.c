/**
 * @file  app_light.c
 * @brief 外置补光（LED 灯板）控制模块实现（补光）
 *
 * 硬件前提（接线与选型见《补光方案_20260919.md》）：
 *   LCD 18pin 座 L3:pin4(GPIO5) → MOS 栅极；pin1(3V3)/pin3(GND) 供电（小电流）；
 *   MOS 栅源之间必须有 100K 下拉（否则深睡时栅极悬空 → 灯微亮/漏电）。
 *
 * ⚠ 本模块只在 `light_en=1` 时才会在拍照流程里点灯；默认 0 → Phase 1 行为不变。
 */
#include "app_light.h"

#include "app_config.h"
#include "bsp/esp-bsp.h"
#include "driver/gpio.h"
#include "driver/ledc.h"
#include "esp_check.h"
#include "esp_io_expander.h"
#include "esp_log.h"
#include "esp_task_wdt.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

/**
 * 微雪扩展芯片私有驱动（组件 waveshare__custom_io_expander_ch32v003）里已有的 PWM 写接口。
 *
 * ⚠ 这里用 extern 声明而不是 #include "custom_io_expander_ch32v003.h"，是为了**不给 main 组件
 *   加 REQUIRES**：main 一旦声明 REQUIRES，就失去"自动依赖全部组件"的行为，会导致
 *   nvs.h / esp_app_desc.h 找不到而编译失败（2026-09-19 实测踩坑，见 CMakeLists 注释）。
 *   该组件是微雪 BSP 的直接依赖，其 .a 必然参与链接，符号可用。
 */
extern esp_err_t custom_io_expander_set_pwm(esp_io_expander_t *handle, uint8_t value);

static const char *TAG = "light";

#define LIGHT_LEDC_MODE      LEDC_LOW_SPEED_MODE    /* ESP32-S3 仅支持低速组（无 HS） */
#define LIGHT_LEDC_TIMER     LEDC_TIMER_0
#define LIGHT_LEDC_CHANNEL   LEDC_CHANNEL_0
#define LIGHT_PWM_FREQ_HZ    20000                  /* 20 kHz：远离相机行频/可听声，波纹不易成带 */
#define LIGHT_PWM_RES        LEDC_TIMER_10_BIT
#define LIGHT_PWM_DUTY_MAX   ((1u << 10) - 1u)      /* 1023 */

/** 等 AE 收敛时的喂狗步长（main 任务订阅了 TWDT） */
#define LIGHT_WAIT_STEP_MS   50

static bool    s_ready = false;
static uint8_t s_level = 0;     /**< 当前占空比对应的亮度 0~255（0=灭） */
static int     s_gpio  = -1;    /**< 已配置的控制脚（-1=未配置） */

/** 分片延时并喂狗（lead 最长几秒，避免 TWDT 告警） */
static void delay_feed(uint32_t ms)
{
    while (ms > 0) {
        uint32_t step = (ms > LIGHT_WAIT_STEP_MS) ? LIGHT_WAIT_STEP_MS : ms;
        vTaskDelay(pdMS_TO_TICKS(step));
        ms -= step;
        if (esp_task_wdt_status(NULL) == ESP_OK) {
            esp_task_wdt_reset();
        }
    }
}

/** 亮度(0~255) → 占空比(0~1023) 并立即生效 */
static esp_err_t apply_level(uint8_t level)
{
    if (!s_ready) {
        return ESP_ERR_INVALID_STATE;
    }
    uint32_t duty = (uint32_t)level * LIGHT_PWM_DUTY_MAX / 255u;
    ESP_RETURN_ON_ERROR(ledc_set_duty(LIGHT_LEDC_MODE, LIGHT_LEDC_CHANNEL, duty),
                        TAG, "ledc_set_duty 失败");
    ESP_RETURN_ON_ERROR(ledc_update_duty(LIGHT_LEDC_MODE, LIGHT_LEDC_CHANNEL),
                        TAG, "ledc_update_duty 失败");
    s_level = level;
    return ESP_OK;
}

esp_err_t app_light_init(void)
{
    const app_cfg_t *cfg = app_config_get();
    int gpio = (int)cfg->light_gpio;

    if (gpio < 0 || gpio > 48) {
        ESP_LOGE(TAG, "补光控制脚配置非法：GPIO%d（light_gpio 应为 0~48）", gpio);
        return ESP_ERR_INVALID_ARG;
    }
    if (gpio == 0 || gpio == 3 || gpio == 45 || gpio == 46) {
        ESP_LOGW(TAG, "⚠ GPIO%d 是 strapping 引脚，复位瞬间电平会影响启动模式，建议改用 GPIO1/5/6", gpio);
    }

    /* 深睡 hold 会跨复位保留：必须先解除，否则引脚被冻结、LEDC 配置不生效 */
    gpio_hold_dis((gpio_num_t)gpio);
    gpio_deep_sleep_hold_dis();

    ledc_timer_config_t tcfg = {
        .speed_mode      = LIGHT_LEDC_MODE,
        .duty_resolution = LIGHT_PWM_RES,
        .timer_num       = LIGHT_LEDC_TIMER,
        .freq_hz         = LIGHT_PWM_FREQ_HZ,
        .clk_cfg         = LEDC_AUTO_CLK,
    };
    ESP_RETURN_ON_ERROR(ledc_timer_config(&tcfg), TAG, "ledc_timer_config 失败");

    ledc_channel_config_t ccfg = {
        .gpio_num   = gpio,
        .speed_mode = LIGHT_LEDC_MODE,
        .channel    = LIGHT_LEDC_CHANNEL,
        .intr_type  = LEDC_INTR_DISABLE,
        .timer_sel  = LIGHT_LEDC_TIMER,
        .duty       = 0,          /* 上电即灭：避免开机闪一下刺到相机 */
        .hpoint     = 0,
    };
    ESP_RETURN_ON_ERROR(ledc_channel_config(&ccfg), TAG, "ledc_channel_config 失败");

    s_ready = true;
    s_gpio  = gpio;
    s_level = 0;

    ESP_LOGI(TAG, "补光通路就绪：GPIO%d，LEDC %d kHz/%d bit，开关=%s，模式=%s，亮度=%u，AE 等待=%ums",
             gpio, LIGHT_PWM_FREQ_HZ / 1000, 10,
             cfg->light_en ? "开" : "关",
             cfg->light_mode ? "会话常亮" : "拍照脉冲",
             (unsigned)cfg->light_level, (unsigned)cfg->light_lead_ms);
    if (!cfg->light_en) {
        ESP_LOGI(TAG, "补光未启用（light_en=0）：可用 `light on/level <v>` 现场试灯，"
                      "`cfg set light_en 1` 后拍照自动补光");
    }
    return ESP_OK;
}

bool app_light_ready(void)
{
    return s_ready;
}

bool app_light_enabled(void)
{
    return app_config_get()->light_en != 0;
}

esp_err_t app_light_set_level(uint8_t level)
{
    return apply_level(level);
}

uint8_t app_light_level(void)
{
    return s_level;
}

esp_err_t app_light_on(void)
{
    return apply_level(app_config_get()->light_level);
}

esp_err_t app_light_off(void)
{
    return apply_level(0);
}

esp_err_t app_light_pulse(uint16_t on_ms)
{
    ESP_RETURN_ON_ERROR(app_light_on(), TAG, "点亮失败");
    delay_feed(on_ms);
    return app_light_off();
}

void app_light_before_capture(void)
{
    const app_cfg_t *cfg = app_config_get();

    if (!cfg->light_en || !s_ready) {
        return;
    }

    if (cfg->light_mode != 0) {
        /* 会话常亮：确保亮着即可（AE 已在该亮度下稳定，抓帧无需额外等待） */
        if (s_level == 0) {
            app_light_on();
        }
        return;
    }

    /* 拍照脉冲：点亮 → 等 AE 收敛 → 返回（调用方随后播快门音并抓帧） */
    if (app_light_on() != ESP_OK) {
        return;   /* 点灯失败不影响拍照 */
    }
    if (cfg->light_lead_ms > 0) {
        delay_feed(cfg->light_lead_ms);
    }
    ESP_LOGI(TAG, "补光已点亮：亮度 %u，AE 等待 %u ms", (unsigned)s_level,
             (unsigned)cfg->light_lead_ms);
}

void app_light_after_capture(void)
{
    const app_cfg_t *cfg = app_config_get();

    if (!cfg->light_en || !s_ready) {
        return;
    }
    if (cfg->light_mode != 0) {
        return;   /* 会话常亮：不在抓帧后熄灭 */
    }
    app_light_off();
}

void app_light_prepare_sleep(void)
{
    if (!s_ready || s_gpio < 0) {
        return;
    }

    /* 1) 占空比清零并停止通道（输出保持恒定低电平） */
    ledc_set_duty(LIGHT_LEDC_MODE, LIGHT_LEDC_CHANNEL, 0);
    ledc_update_duty(LIGHT_LEDC_MODE, LIGHT_LEDC_CHANNEL);
    ledc_stop(LIGHT_LEDC_MODE, LIGHT_LEDC_CHANNEL, 0);

    /* 2) 转普通 GPIO 输出低并 hold：深睡期间栅极被钉在低电平，
     *    即便外部没焊 100K 下拉也不会因悬空导致 MOS 误导通/灯微亮。 */
    gpio_set_direction((gpio_num_t)s_gpio, GPIO_MODE_OUTPUT);
    gpio_set_level((gpio_num_t)s_gpio, 0);
    gpio_hold_en((gpio_num_t)s_gpio);
    gpio_deep_sleep_hold_en();

    s_level = 0;
    ESP_LOGI(TAG, "补光控制脚 GPIO%d 已固定低电平并 hold（深睡不耗电）", s_gpio);
}

esp_err_t app_light_expander_pwm(uint8_t value)
{
    esp_io_expander_handle_t io = bsp_get_io_expander_handle();
    if (io == NULL) {
        ESP_LOGE(TAG, "扩展芯片未初始化，PWM 自检不可用");
        return ESP_ERR_INVALID_STATE;
    }
    ESP_RETURN_ON_ERROR(custom_io_expander_set_pwm(io, value), TAG,
                        "写扩展芯片 PWM 寄存器(0x05) 失败");
    ESP_LOGI(TAG, "已向扩展芯片写 PWM=0~255 中的 %u（落到 EXIO_PWM1/LCD_BL 脚，需万用表实测确认）",
             (unsigned)value);
    return ESP_OK;
}
