/**
 * @file  app_cmd.c
 * @brief 串口命令行模块实现
 *
 * 实现说明（重要，踩坑结论）：
 *   本板主控台为 USB-Serial-JTAG。该 console 的**非阻塞** read 路径依赖
 *   usb_serial_jtag 驱动上报的"可用字节数"（usb_serial_jtag_get_read_bytes_available），
 *   而只注册 VFS+LL 时该计数恒为 0 → 非阻塞 read 永远读不到数据（表现为：主机写入被阻塞、
 *   设备收不到输入）。**阻塞式 read() 走 LL 直读 RX FIFO**，可靠。
 *   因此本模块用一个小任务做阻塞 read（而非在主循环里非阻塞轮询）。
 *   另注：不能用 stdio 的 fgetc()——非阻塞 read 返回 0 会被 stdio 当作 EOF 并置 EOF 标志。
 */
#include "app_cmd.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "app_audio.h"
#include "app_config.h"
#include "app_power.h"
#include "app_session.h"
#include "app_storage.h"
#include "bsp/esp-bsp.h"
#include "esp_app_desc.h"
#include "esp_err.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "cmd";

#define LINE_MAX 128
#define ARG_MAX  8

static char s_line[LINE_MAX];
static int  s_len = 0;

static app_cmd_snap_fn  s_snap = NULL;
static app_cmd_sleep_fn s_sleep = NULL;
static char            s_last_result[64] = "无";
static bool            s_reset_pending = false;

static void cmd_task(void *arg);

void app_cmd_set_snap_handler(app_cmd_snap_fn fn)
{
    s_snap = fn;
}

void app_cmd_set_sleep_handler(app_cmd_sleep_fn fn)
{
    s_sleep = fn;
}

void app_cmd_set_last_result(const char *s)
{
    if (s == NULL) {
        return;
    }
    strncpy(s_last_result, s, sizeof(s_last_result) - 1);
    s_last_result[sizeof(s_last_result) - 1] = '\0';
}

/* ---------------- 各命令 ---------------- */

static void cmd_help(void)
{
    printf("可用命令：\n"
           "  help               显示本帮助\n"
           "  info               固件 / IDF / 编译信息\n"
           "  status             电量(ADC) / SD 容量 / 拍摄计数 / 上次结果\n"
           "  snap               立即执行一次拍照\n"
           "  tone <0-%d>        试听指定提示音\n"
           "  cfg list           列出当前配置\n"
           "  cfg set <键> <值>   修改配置并写 NVS\n"
           "  reset              恢复出厂默认（需再输 reset yes 确认）\n"
           "  sleep              立即入睡（资源回收+降流→深睡，串口随后断开）\n"
           "  btn [秒]           监听拍照键电平（键位/极性自检，默认 8 s）\n",
           TONE_COUNT - 1);
}

static void cmd_info(void)
{
    const esp_app_desc_t *d = esp_app_get_description();
    printf("项目     : %s\n", d->project_name);
    printf("版本     : %s\n", d->version);
    printf("ESP-IDF  : %s\n", d->idf_ver);
    printf("编译时间 : %s %s\n", d->date, d->time);
    printf("目标芯片 : %s\n", CONFIG_IDF_TARGET);
}

static void cmd_status(void)
{
    const app_cfg_t *c = app_config_get();

    printf("电量 ADC : %u（扩展芯片 BAT_ADC 原始值）\n", (unsigned)bsp_get_io_expander_adc());
    printf("拍照键   : IO%d 电平=%d（1=松开 / 0=按下；低有效）\n",
           APP_BUTTON_PIN, app_power_button_level());
    printf("会话计时 : 剩余 %u s / 统一时长 %u s%s\n",
           (unsigned)app_session_remain_s(), (unsigned)app_session_keep_s(),
           app_session_active() ? "" : "（未开始）");

    uint64_t total = 0, freeb = 0;
    if (app_storage_get_space(&total, &freeb) == ESP_OK) {
        printf("SD 卡    : 总 %llu MB / 剩余 %llu MB\n",
               (unsigned long long)(total >> 20), (unsigned long long)(freeb >> 20));
    } else {
        printf("SD 卡    : 未挂载\n");
    }
    printf("拍摄计数 : %u\n", (unsigned)c->file_seq);
    printf("上次结果 : %s\n", s_last_result);
}

static void cmd_snap(void)
{
    if (s_snap == NULL) {
        printf("拍照回调未注册\n");
        return;
    }
    printf("执行一次拍照…\n");
    s_snap();
}

static void cmd_sleep(void)
{
    if (s_sleep == NULL) {
        printf("入睡回调未注册\n");
        return;
    }
    s_sleep();
    /* 正常不会返回；返回说明未真正进入深睡 */
    printf("（未进入深睡，入睡回调已返回）\n");
}

/** 按键自检：监听拍照键电平变化 N 秒（默认 8 s，1=松开 / 0=按下），用于核对键位与极性 */
static void cmd_btn(int argc, char **argv)
{
    int secs = (argc >= 2) ? atoi(argv[1]) : 8;
    if (secs < 1) {
        secs = 1;
    } else if (secs > 60) {
        secs = 60;
    }

    printf("监听拍照键 IO%d 共 %d s（1=松开 / 0=按下），请按键…\n", APP_BUTTON_PIN, secs);
    int last = app_power_button_level();
    printf("初始电平 = %d\n", last);

    int changes = 0;
    const TickType_t t0 = xTaskGetTickCount();
    while ((xTaskGetTickCount() - t0) < pdMS_TO_TICKS(secs * 1000)) {
        int lv = app_power_button_level();
        if (lv != last) {
            printf("  t=%lums  IO%d: %d -> %d\n",
                   (unsigned long)((xTaskGetTickCount() - t0) * portTICK_PERIOD_MS),
                   APP_BUTTON_PIN, last, lv);
            last = lv;
            changes++;
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    printf("监听结束：电平变化 %d 次\n", changes);
}

static void cmd_tone(int argc, char **argv)
{
    if (argc < 2) {
        printf("用法：tone <0-%d>\n", TONE_COUNT - 1);
        return;
    }
    int id = atoi(argv[1]);
    if (id < 0 || id >= TONE_COUNT) {
        printf("id 越界（有效 0-%d）\n", TONE_COUNT - 1);
        return;
    }
    printf("播放提示音 #%d…\n", id);
    app_audio_play((tone_id_t)id);
}

static void cmd_cfg(int argc, char **argv)
{
    if (argc >= 2 && strcmp(argv[1], "list") == 0) {
        const app_cfg_t *c = app_config_get();
        printf("jpeg_quality  = %u\n", (unsigned)c->jpeg_quality);
        printf("frame_size    = %u\n", (unsigned)c->frame_size);
        printf("af_settle_ms  = %u\n", (unsigned)c->af_settle_ms);
        printf("low_space_pct = %u\n", (unsigned)c->low_space_pct);
        printf("file_seq      = %u\n", (unsigned)c->file_seq);
        printf("audio_vol     = %u\n", (unsigned)c->audio_vol);
        printf("audio_mute    = %u\n", (unsigned)c->audio_mute);
        printf("hold_awake_s  = %u\n", (unsigned)c->hold_awake_s);
    } else if (argc >= 4 && strcmp(argv[1], "set") == 0) {
        uint32_t v = (uint32_t)strtoul(argv[3], NULL, 0);
        esp_err_t err = app_config_set(argv[2], v);
        if (err == ESP_OK) {
            printf("OK：%s = %u（已写 NVS）\n", argv[2], (unsigned)v);
        } else {
            printf("失败：%s（%s）\n", argv[2], esp_err_to_name(err));
        }
    } else {
        printf("用法：cfg list | cfg set <键> <值>\n");
    }
}

static void cmd_reset(int argc, char **argv)
{
    if (argc >= 2 && strcmp(argv[1], "yes") == 0) {
        if (!s_reset_pending) {
            printf("请先输入 reset 再确认。\n");
            return;
        }
        esp_err_t err = app_config_reset();
        s_reset_pending = false;
        if (err == ESP_OK) {
            printf("已恢复出厂默认\n");
        } else {
            printf("失败：%s\n", esp_err_to_name(err));
        }
        return;
    }
    s_reset_pending = true;
    printf("⚠ 将清空 NVS 并恢复默认。再输入 reset yes 确认。\n");
}

/* ---------------- 解析与分发 ---------------- */

static int split_args(char *line, char *argv[], int max)
{
    int n = 0;
    char *p = line;
    while (*p && n < max) {
        while (*p == ' ' || *p == '\t') {
            p++;
        }
        if (*p == '\0') {
            break;
        }
        argv[n++] = p;
        while (*p && *p != ' ' && *p != '\t') {
            p++;
        }
        if (*p) {
            *p++ = '\0';
        }
    }
    return n;
}

static void dispatch(char *line)
{
    char *argv[ARG_MAX];
    int argc = split_args(line, argv, ARG_MAX);
    if (argc == 0) {
        return;
    }

    if (strcmp(argv[0], "help") == 0) {
        cmd_help();
    } else if (strcmp(argv[0], "info") == 0) {
        cmd_info();
    } else if (strcmp(argv[0], "status") == 0) {
        cmd_status();
    } else if (strcmp(argv[0], "snap") == 0) {
        cmd_snap();
    } else if (strcmp(argv[0], "tone") == 0) {
        cmd_tone(argc, argv);
    } else if (strcmp(argv[0], "cfg") == 0) {
        cmd_cfg(argc, argv);
    } else if (strcmp(argv[0], "reset") == 0) {
        cmd_reset(argc, argv);
    } else if (strcmp(argv[0], "sleep") == 0) {
        cmd_sleep();
    } else if (strcmp(argv[0], "btn") == 0) {
        cmd_btn(argc, argv);
    } else {
        printf("未知命令：%s（输入 help）\n", argv[0]);
    }
}

/** 处理从串口读到的字节：回显 + 组行 + 分发 */
static void process_bytes(const uint8_t *buf, int n)
{
    for (int i = 0; i < n; i++) {
        int c = buf[i];
        if (c == '\r' || c == '\n') {
            if (s_len > 0) {
                s_line[s_len] = '\0';
                printf("\n");
                dispatch(s_line);
                s_len = 0;
            }
            printf("> ");
        } else if (c == 8 || c == 127) {   /* 退格 */
            if (s_len > 0) {
                s_len--;
                printf("\b \b");
            }
        } else if (c >= 32 && c < 127) {
            if (s_len < LINE_MAX - 1) {
                s_line[s_len++] = (char)c;
                printf("%c", c);           /* 回显 */
            }
        }
    }
}

/** 独立命令任务：阻塞读取 stdin（走 LL 直读 USB-JTAG RX FIFO） */
static void cmd_task(void *arg)
{
    (void)arg;
    uint8_t buf[64];
    for (;;) {
        int n = read(STDIN_FILENO, buf, sizeof(buf));
        if (n > 0) {
            process_bytes(buf, n);
        } else {
            vTaskDelay(pdMS_TO_TICKS(20));
        }
    }
}

/* ---------------- 对外接口 ---------------- */

esp_err_t app_cmd_init(void)
{
    BaseType_t ok = xTaskCreate(cmd_task, "cmd", 4096, NULL, 5, NULL);
    if (ok != pdPASS) {
        ESP_LOGE(TAG, "创建命令行任务失败");
        return ESP_FAIL;
    }

    printf("\n[cmd] 串口命令行就绪。输入 help 查看命令。\n> ");
    ESP_LOGI(TAG, "命令行就绪（独立任务阻塞读取 stdin）");
    return ESP_OK;
}
