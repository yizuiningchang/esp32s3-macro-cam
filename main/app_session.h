/**
 * @file  app_session.h
 * @brief 统一计时策略（v1.6）：唤醒会话的开始 / 重置 / 超时判定 / 剩余秒数
 *
 * / 4.2 / 4.7：
 *   - 时长**统一取自** NVS 键 `hold_awake_s`（默认 300 s，NVS 可配，量产支持改），
 *     本模块不再自行定义时长，避免各处硬编码。
 *   - 语义为"无操作超时"：**上电 / 深睡唤醒 / 每一次拍照（成功或失败）** 都调用
 *     `app_session_refresh()` 把 deadline 重置为"现在 + 时长"。
 *   - 超时后由调用方（main）播放「即将休眠」并进入深睡。
 *   - 供**睡眠逻辑**与后续计时功能共用。
 *
 * 实现：基于 FreeRTOS tick 的绝对 deadline，比较用有符号差值，
 *       天然容忍 tick 回绕（1 kHz 下约 49.7 天）。
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** @brief 开始（或重新开始）会话计时：deadline = 现在 + `hold_awake_s` */
void app_session_begin(void);

/** @brief 刷新计时（与 begin 同义；语义化别名，用于"每次拍照后重置"） */
void app_session_refresh(void);

/** @brief 计时是否已到点（无操作超时）。未开始时恒为 false */
bool app_session_expired(void);

/** @brief 剩余秒数（0 = 已到点或未开始；供 status / 调试使用） */
uint32_t app_session_remain_s(void);

/** @brief 当前统一时长（秒），直接来自 NVS 配置 */
uint32_t app_session_keep_s(void);

/** @brief 计时是否已启动 */
bool app_session_active(void);

#ifdef __cplusplus
}
#endif
