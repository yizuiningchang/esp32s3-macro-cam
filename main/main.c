/**
 * @file  main.c
 * @brief ESP32-S3 微距拍摄终端 — Phase 1 主流程
 *
 * 唤醒会话模型（深睡为常态）：
 *   上电/复位 或 深睡中被 PWR 唤醒 → 播「叮」→ 进入**唤醒会话**（摄像头常驻、统一计时）
 *   ├─ 短按 PWR → 拍照 + 保存 + 声音/LED 反馈 → **刷新计时**
 *   ├─ 长按 PWR(>1.5 s) → 播「即将休眠」→ **立即入睡**
 *   └─ 无操作满 `hold_awake_s`（默认 300 s）→ 剩余 10 s 播「即将休眠」→ **播完自动入睡**
 *
 * 按键语义：深睡中**首次按键只唤醒、不拍照**（被消费），
 * 之后再按才拍照；会话内可连续多拍，每次拍照都重置计时。
 *
 * 统一计时策略（v1.6）：时长取自 NVS `hold_awake_s`，由 `app_session` 模块提供，
 * 供睡眠逻辑与后续计时功能共用。
 */
#include <inttypes.h>
#include <stdbool.h>
#include <string.h>

#include "app_audio.h"
#include "app_camera.h"
#include "app_cmd.h"
#include "app_config.h"
#include "app_led.h"
#include "app_light.h"
#include "app_power.h"
#include "app_session.h"
#include "app_storage.h"
#include "bsp/esp-bsp.h"
#include "esp_app_desc.h"
#include "esp_log.h"
#include "esp_task_wdt.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "main";

/** 开发期：冷启动（上电/复位）进入会话时**先拍一张**，便于调试。
 *  定版值 0（仅按键才拍照）。 */
#define APP_CAPTURE_ON_COLD_BOOT 0

/** 定版值 0：会话超时后**真进深睡**。
 *  开发期曾为 1——超时也不睡，保持 USB-Serial-JTAG 在线看日志。
 *  注意：深睡会切断 USB-Serial-JTAG，主机侧串口消失，属**预期行为**，不是故障。 */
#define APP_DEBUG_KEEP_AWAKE 0

/** 长按判定阈值：按住 ≥ 此值 → 立即休眠 */
#define APP_LONG_PRESS_MS 1500

/** 睡眠预警提前量：会话剩余 ≤ 此值 → 播「即将休眠」 */
#define APP_SLEEP_WARN_S 10

/** 单次拍照结果 */
typedef enum {
    CAP_OK = 0,
    CAP_ERR_MOUNT,     /**< TF 卡挂载失败 */
    CAP_ERR_SPACE,     /**< 容量低于阈值 */
    CAP_ERR_CAMERA,    /**< 摄像头初始化/抓帧失败 */
    CAP_ERR_WRITE,     /**< 写卡失败 */
} cap_result_t;

/** 距 t0 的毫秒数 */
static inline uint32_t since_ms(int64_t t0)
{
    return (uint32_t)((esp_timer_get_time() - t0) / 1000);
}

/** 喂狗：仅当**当前任务已订阅** TWDT 时才调用，避免 cmd 任务调用时报 "task not found" */
static inline void wdt_feed(void)
{
    if (esp_task_wdt_status(NULL) == ESP_OK) {
        esp_task_wdt_reset();
    }
}

/**
 * @brief 单次拍照线性流程（含各步骤耗时打点）
 *
 * 会话期间摄像头**常驻**（由会话入口 init 一次），故本函数不再 init/deinit 摄像头；
 * 睡眠前由 `app_power_prepare_sleep()` 统一释放。
 *
 * @param[out] out_path 成功时回填落卡路径
 */
static cap_result_t run_capture_flow(char *out_path, size_t out_path_len)
{
    const app_cfg_t *cfg = app_config_get();
    const int64_t t_total = esp_timer_get_time();
    int64_t t_step = t_total;
    esp_err_t err;

    wdt_feed();

    /* --- 1) 挂载 TF 卡（失败重试间隔 50 ms） --- */
    err = app_storage_mount();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "TF 卡不可用，终止本次拍照（+%ums）", (unsigned)since_ms(t_total));
        return CAP_ERR_MOUNT;
    }
    ESP_LOGI(TAG, "耗时：挂卡 %ums", (unsigned)since_ms(t_step));
    t_step = esp_timer_get_time();

    /* --- 2) 容量检查 --- */
    if (!app_storage_space_ok(cfg->low_space_pct)) {
        ESP_LOGE(TAG, "剩余容量低于 %u%%，停拍保护历史照片（+%ums）",
                 (unsigned)cfg->low_space_pct, (unsigned)since_ms(t_total));
        return CAP_ERR_SPACE;
    }

    /* --- 3) 摄像头就绪（会话内已常驻；若未初始化则此处 init） --- */
    err = app_camera_init();
    if (err != ESP_OK) {
        return CAP_ERR_CAMERA;
    }
    ESP_LOGI(TAG, "耗时：摄像头就绪 %ums（常驻时≈0）", (unsigned)since_ms(t_step));
    t_step = esp_timer_get_time();
    wdt_feed();

    /* --- 4) 补光 + 快门音 + 抓帧 ---
     *  补光先亮：脉冲模式下等待 `light_lead_ms` 让 AE 收敛到亮场（灯亮 → 曝光降 → 噪声降）；
     *  随后播快门音（需求方定稿时机），播放阻塞期间摄像头 DMA 持续出流，等效"丢帧等待 AE 收敛"，
     *  故 app_camera_capture 内仅 init 后首张单独丢帧（其余张直接抓）。 */
    app_light_before_capture();
    app_audio_play(TONE_SHUTTER);   /* Canon A1 真实快门「咔-嚓」（CC0，v1.7） */
    camera_fb_t *fb = app_camera_capture();
    app_light_after_capture();      /* 抓帧拿到即灭灯（写卡不需要光） */
    if (fb == NULL) {
        ESP_LOGE(TAG, "抓帧失败");
        return CAP_ERR_CAMERA;
    }
    ESP_LOGI(TAG, "耗时：抓帧 %ums", (unsigned)since_ms(t_step));
    ESP_LOGI(TAG, "抓帧成功：%u 字节 %ux%u %s",
             (unsigned)fb->len, (unsigned)fb->width, (unsigned)fb->height,
             fb->format == PIXFORMAT_JPEG ? "JPEG" : "非JPEG");
    app_camera_log_ae();   /* v1.9：本帧 AE 落点（曝光/增益）——竖纹归因取证 */
    t_step = esp_timer_get_time();

    /* --- 5) 写卡 --- */
    esp_err_t werr = app_storage_save_jpeg(fb->buf, fb->len, out_path, out_path_len);
    app_camera_return(fb);   /* 必须先归还帧缓冲再判断结果 */
    if (werr != ESP_OK) {
        return CAP_ERR_WRITE;
    }
    ESP_LOGI(TAG, "耗时：写卡 %ums", (unsigned)since_ms(t_step));

    ESP_LOGI(TAG, "拍照流程总耗时 %ums（按下→落卡）", (unsigned)since_ms(t_total));
    return CAP_OK;
}

/**
 * @brief 结果反馈：声音为主 + LED 辅助 + 串口日志
 */
static void report_result(cap_result_t result, const char *path)
{
    switch (result) {
    case CAP_OK:
        ESP_LOGI(TAG, "拍照成功：%s", path);
        app_audio_play(TONE_SUCCESS);      /* 双音上行「叮咚」 */
        app_led_show(LED_MODE_ON);         /* 成功不单独指示：保持常亮（v1.2） */
        app_cmd_set_last_result("成功");
        break;
    case CAP_ERR_MOUNT:
        ESP_LOGE(TAG, "TF 卡缺失 → 三短哔 + LED 闪 3 次");
        app_audio_play(TONE_SD_ERROR);     /* 三短「哔哔哔」 */
        app_led_show(LED_MODE_ALERT);      /* 闪 3 次后恢复常亮 */
        app_cmd_set_last_result("失败：TF 卡缺失");
        break;
    case CAP_ERR_SPACE:
        ESP_LOGE(TAG, "容量不足 → 三短哔 + LED 闪 3 次");
        app_audio_play(TONE_SD_ERROR);     /* 三短「哔哔哔」 */
        app_led_show(LED_MODE_ALERT);      /* 闪 3 次后恢复常亮 */
        app_cmd_set_last_result("失败：容量不足");
        break;
    case CAP_ERR_CAMERA:
        ESP_LOGE(TAG, "摄像头异常 → 下行双音 + LED 闪 3 次");
        app_audio_play(TONE_FAIL);         /* 双音下行「嗡—嘟」 */
        app_led_show(LED_MODE_ALERT);      /* 闪 3 次后恢复常亮 */
        app_cmd_set_last_result("失败：摄像头异常");
        break;
    case CAP_ERR_WRITE:
        ESP_LOGE(TAG, "写卡失败 → 下行双音 + LED 闪 3 次");
        app_audio_play(TONE_FAIL);         /* 双音下行「嗡—嘟」 */
        app_led_show(LED_MODE_ALERT);      /* 闪 3 次后恢复常亮 */
        app_cmd_set_last_result("失败：写卡失败");
        break;
    default:
        break;
    }
}

/** cmd 的 snap 命令回调：立即执行一次完整拍照流程 */
static void do_snap(void)
{
    char path[64] = {0};
    cap_result_t r = run_capture_flow(path, sizeof(path));
    report_result(r, path);
    app_storage_unmount();     /* 用完即卸卡 */
}

/** cmd 的 sleep 命令回调：播「即将休眠」→ 真深睡（调试/电流实测用） */
static void do_sleep(void)
{
    printf("执行入睡：播放「即将休眠」→ 资源回收/降流 → 深睡（此后 USB 串口将断开）\n");
    app_audio_play(TONE_SLEEP_WARN);
    app_power_enter_deep_sleep();
}

void app_main(void)
{
    /* ===================== 初始化 ===================== */
    ESP_ERROR_CHECK(app_config_init());
    app_power_log_wakeup_cause();

    /* 拍照键（PWR/GPIO15）数字输入：会话内短按/长按判定都靠它 */
    ESP_ERROR_CHECK(app_power_button_init());

    /* 共享 I2C 总线 + 扩展 IO（摄像头 SCCB / ES8311 / CH32V003 三方共用） */
    ESP_ERROR_CHECK(bsp_i2c_init());
    ESP_ERROR_CHECK(bsp_io_expander_init());

    /* 音频通路（I2S + ES8311 + 功放），失败仅告警不影响拍照 */
    if (app_audio_init() != ESP_OK) {
        ESP_LOGW(TAG, "音频初始化失败，本次运行静默（不影响拍照）");
    }

    /* USER LED（EXIO6），置为常亮工作态；失败仅告警不影响拍照 */
    if (app_led_init() != ESP_OK) {
        ESP_LOGW(TAG, "USER LED 初始化失败（不影响拍照）");
    }

    /* 外置补光（LCD 座空闲 GPIO + LEDC PWM）；默认 light_en=0 不点灯，失败不影响拍照 */
    if (app_light_init() != ESP_OK) {
        ESP_LOGW(TAG, "补光通路初始化失败（不影响拍照；检查 light_gpio 配置）");
    }

    /* 串口命令行（调试/维护）：注册 snap / sleep 回调 */
    app_cmd_init();
    app_cmd_set_snap_handler(do_snap);
    app_cmd_set_sleep_handler(do_sleep);

    /* main 任务订阅 TWDT：长流程/长等待期间周期喂狗（PANIC 未开 → 仅告警不重启） */
    if (esp_task_wdt_add(NULL) != ESP_OK) {
        ESP_LOGW(TAG, "main 任务订阅 TWDT 失败（继续运行，仅少一层兜底）");
    }

    const bool by_button = app_power_woke_by_button();

    /* 构建标记：确认"本次烧录是否生效"（main.c 每次改动都会刷新） */
    const esp_app_desc_t *desc = esp_app_get_description();
    ESP_LOGI(TAG, "构建标记：main.c @ %s %s，固件版本 %s", __DATE__, __TIME__, desc->version);
    ESP_LOGI(TAG, "微距拍摄终端启动：唤醒源=%s，电量 ADC=%u",
             by_button ? "按键" : "上电/复位", (unsigned)app_power_battery_adc());

    /* 低电量告警：阈值未标定 → 当前 app_power_battery_low() 恒 false */
    if (app_power_battery_low()) {
        ESP_LOGW(TAG, "电量偏低 → 播放低电量提示音");
        app_audio_play(TONE_LOW_BATTERY);
    }

    /* ===================== 唤醒会话（v1.6） ===================== */
    /* 开机/唤醒音：上电与深睡唤醒都播 */
    app_audio_play(TONE_WAKEUP);

    /* 按键交互：深睡中被按下的那一次按键**只用于唤醒、不拍照**，
     * 先等它松开，避免主循环立刻把它当成"短按拍照"。 */
    if (by_button) {
        app_power_wait_button_release(2000);
    }

    /* 会话期间摄像头常驻：进入会话先 init 一次，之后每张不再重复
     * （规避按下后 ~720 ms 初始化延时与 AE/AWB 未收敛的画质问题）。 */
    if (app_camera_init() != ESP_OK) {
        ESP_LOGW(TAG, "会话内摄像头初始化失败，首次拍照会再重试");
    }

#if APP_CAPTURE_ON_COLD_BOOT
    if (!by_button) {
        ESP_LOGW(TAG, "冷启动调试模式：先拍一张（定版请置 0）");
        char p[64] = {0};
        report_result(run_capture_flow(p, sizeof(p)), p);
        app_storage_unmount();
    }
#endif

    /* 统一计时：上电 / 唤醒 / 每次拍照都重置 */
    app_session_begin();
    const uint32_t keep_s = app_session_keep_s();
    ESP_LOGI(TAG, "进入唤醒会话：保持 %u s；短按 PWR 拍照并刷新计时，长按 >%d ms 立即休眠",
             (unsigned)keep_s, APP_LONG_PRESS_MS);

    bool warn_played  = false;   /* 本轮计时是否已播过「即将休眠」 */
    bool long_pressed = false;   /* 是否因长按而退出 */

    for (;;) {
        wdt_feed();

        /* ---- 按键：短按 = 拍照并刷新计时；长按 = 立即休眠 ---- */
        if (app_power_button_pressed()) {          /* 含 50 ms 消抖 */
            if (app_power_wait_button_release_long(APP_LONG_PRESS_MS)) {
                long_pressed = true;
                ESP_LOGI(TAG, "检测到长按（>%d ms）→ 立即休眠", APP_LONG_PRESS_MS);
                break;
            }

            ESP_LOGI(TAG, "拍照键短按 → 执行一次拍照");
            char path[64] = {0};
            cap_result_t r = run_capture_flow(path, sizeof(path));
            report_result(r, path);
            app_storage_unmount();     /* 用完即卸卡（防异常复位时卡停留挂载态） */

            /* 统一计时：**成功或失败都刷新**；预警标记一并复位 */
            app_session_refresh();
            warn_played = false;
            continue;
        }

        /* ---- 自动休眠：剩余 ≤ 10 s 播「即将休眠」（仅一次）；到点静默入睡 ---- */
        if (!warn_played && app_session_remain_s() <= APP_SLEEP_WARN_S) {
            ESP_LOGI(TAG, "会话剩余 %u s → 播「即将休眠」预警音",
                     (unsigned)app_session_remain_s());
            app_audio_play(TONE_SLEEP_WARN);   /* 阻塞播放，播完才继续 */
            warn_played = true;
        }

        if (app_session_expired()) {
#if APP_DEBUG_KEEP_AWAKE
            ESP_LOGW(TAG, "调试模式：会话超时但不入睡，重新计时");
            app_session_begin();
            warn_played = false;
            continue;
#else
            ESP_LOGI(TAG, "会话超时（无操作满 %u s）→ 自动休眠", (unsigned)keep_s);
            break;
#endif
        }

        vTaskDelay(pdMS_TO_TICKS(50));
    }

    /* ===================== 入睡 ===================== */
    if (long_pressed) {
        /* 长按：按设计播「即将休眠」后立即入睡（阻塞播放，播完才睡） */
        app_audio_play(TONE_SLEEP_WARN);
    }
    /* 超时路径：预警音已在 T-10 s 播过，此处静默入睡 */

    /* 内部会：摄像头 deinit + 断言 PWDN / 卸载 TF 卡 / 关功放 + ES8311 低功耗 / 灭灯 / 配 ext1 唤醒 */
    app_power_enter_deep_sleep();
}
