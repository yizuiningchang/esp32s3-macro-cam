/**
 * @file  app_session.c
 * @brief 统一计时策略实现
 */
#include "app_session.h"

#include "app_config.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "session";

/** 安全上限：避免配置成超大值导致 tick 溢出（300 s 的 100 倍 = 8.3 h，足够） */
#define SESSION_KEEP_MAX_S 30000u

static TickType_t s_deadline = 0;
static uint32_t   s_keep_s   = 0;
static bool       s_active   = false;

uint32_t app_session_keep_s(void)
{
    return (uint32_t)app_config_get()->hold_awake_s;
}

void app_session_begin(void)
{
    uint32_t keep = app_session_keep_s();
    if (keep == 0) {
        keep = 1;   /* 0 视为非法配置，兜底 1 s，避免"立刻睡死"无法操作 */
        ESP_LOGW(TAG, "hold_awake_s=0 非法，兜底按 1 s 计时");
    } else if (keep > SESSION_KEEP_MAX_S) {
        ESP_LOGW(TAG, "hold_awake_s=%u 过大，钳到 %u s", (unsigned)keep,
                 (unsigned)SESSION_KEEP_MAX_S);
        keep = SESSION_KEEP_MAX_S;
    }

    s_keep_s   = keep;
    s_deadline = xTaskGetTickCount() + pdMS_TO_TICKS(keep * 1000u);
    s_active   = true;

    ESP_LOGI(TAG, "会话计时开始/重置：%u s", (unsigned)keep);
}

void app_session_refresh(void)
{
    app_session_begin();   /* 语义相同：重置 deadline */
}

/** 剩余毫秒（可为负；未开始时返回 0） */
static int32_t session_remain_ms(void)
{
    if (!s_active) {
        return 0;
    }
    /* 有符号差值比较：容忍 tick 回绕 */
    return (int32_t)(s_deadline - xTaskGetTickCount()) * (int32_t)portTICK_PERIOD_MS;
}

bool app_session_expired(void)
{
    return s_active && (session_remain_ms() <= 0);
}

uint32_t app_session_remain_s(void)
{
    int32_t ms = session_remain_ms();
    if (ms <= 0) {
        return 0;
    }
    return (uint32_t)(ms / 1000);
}

bool app_session_active(void)
{
    return s_active;
}
