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
#include "app_camera.h"
#include "app_config.h"
#include "app_light.h"
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
           "  cam show           回读摄像头实际生效的画质参数与 AE 落点\n"
           "  cam apply          把 NVS 画质参数下发到传感器（改完 cfg set 执行）\n"
           "  light [on|off]     补光开关状态 / 手动点亮熄灭（REQ）\n"
           "  light level <0-255> 现场调补光亮度（不写 NVS）\n"
           "  light pulse [ms]   试灯：亮 ms 毫秒后自动灭（默认 500）\n"
           "  light exp <0-255>  自检：写扩展芯片 PWM 寄存器（LCD_BL 脚）\n"
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
    printf("补光     : %s（light_en=%u / GPIO%u / 模式=%s / 亮度 %u / AE 等待 %ums）\n",
           app_light_enabled() ? "开" : "关", (unsigned)c->light_en, (unsigned)c->light_gpio,
           c->light_mode ? "会话常亮" : "拍照脉冲",
           (unsigned)app_light_level(), (unsigned)c->light_lead_ms);
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
        printf("-- 画质调参（255/65535 = 不干预）--\n");
        printf("gain_ceil     = %u   # AGC 上限码=增益x16；248 出厂(15.5x)、128=8x、64=4x、32=2x\n",
               (unsigned)c->gain_ceil);
        printf("aec_limit     = %u   # AEC 曝光上限(行)；65535=出厂984行(UXGA半帧)；UXGA暗场建议 1960\n",
               (unsigned)c->aec_limit);
        printf("ae_level      = %u   # 0..10（实为 -5..+5，5=中性）\n", (unsigned)c->ae_level);
        printf("denoise       = %u   # 0..8\n", (unsigned)c->denoise);
        printf("sharpness     = %u   # 0..6（实为 -3..+3，3=中性）；只改观感，不救失焦\n",
               (unsigned)c->sharpness);
        printf("ae_lock       = %u   # 1=锁定曝光与增益\n", (unsigned)c->ae_lock);
        printf("aec_value     = %u   # 锁定时曝光行 0..1200\n", (unsigned)c->aec_value);
        printf("agc_gain      = %u   # 锁定时增益档 0..30\n", (unsigned)c->agc_gain);
        printf("wb_lock       = %u   # 1=锁定白平衡\n", (unsigned)c->wb_lock);
        printf("-- 外置补光（REQ）--\n");
        printf("light_en      = %u   # 0=关（默认）1=拍照自动补光\n", (unsigned)c->light_en);
        printf("light_mode    = %u   # 0=拍照脉冲（默认）1=会话常亮\n", (unsigned)c->light_mode);
        printf("light_level   = %u   # 亮度 0..255（抓帧建议 255 满亮）\n", (unsigned)c->light_level);
        printf("light_lead_ms = %u   # 点亮→抓帧 等待，等 AE 收敛\n", (unsigned)c->light_lead_ms);
        printf("light_gpio    = %u   # 控制脚（5=LCD 座 pin4；改后需重启生效）\n", (unsigned)c->light_gpio);
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

/**
 * @brief 画质调参命令（v1.8，暗场竖纹治理）
 *
 *   cam show    从传感器回读实际生效值（含 AE 落点）—— 确认参数真的落到寄存器
 *   cam apply   把 NVS 里的调参重新下发（cfg set 之后执行，无需重编/重启）
 */
static void cmd_cam(int argc, char **argv)
{
    if (argc >= 2 && strcmp(argv[1], "show") == 0) {
        app_camera_dump_tuning();
        return;
    }
    if (argc >= 2 && strcmp(argv[1], "apply") == 0) {
        esp_err_t err = app_camera_apply_tuning();
        if (err == ESP_OK) {
            printf("已下发。用 cam show 核对。\n");
        } else {
            printf("下发失败：%s\n", esp_err_to_name(err));
        }
        return;
    }
    printf("用法：cam show | cam apply\n"
           "  配合 cfg set 使用，例如：\n"
           "    cfg set gain_ceil 64      # AGC 上限收到 4x（压竖纹）\n"
           "    cfg set aec_limit 1960    # 曝光上限放宽到近满帧（UXGA 暗场：曝光换增益）\n"
           "    cfg set ae_level 3        # AE 目标下调两档\n"
           "    cfg set denoise 4         # 开降噪\n"
           "    cfg set sharpness 6       # ISP 锐度 +3（6 档制，3=中性）\n"
           "    cam apply\n"
           "    cam show\n");
}

/**
 * @brief 补光命令（REQ）：现场试灯 / 自检，不改 NVS（除 cfg set light_*）
 *
 *   light              显示当前状态
 *   light on | off     按 NVS 亮度点亮 / 熄灭
 *   light level <0-255> 直接改亮度（当场生效，不写 NVS）
 *   light pulse <ms>   试灯：亮 ms 毫秒后自动灭（默认 500）
 *   light exp <0-255>  把值写到扩展芯片 PWM 寄存器（EXIO_PWM1/LCD_BL 脚）——A/B 自检用
 */
static void cmd_light(int argc, char **argv)
{
    if (argc >= 2 && strcmp(argv[1], "on") == 0) {
        esp_err_t err = app_light_on();
        if (err == ESP_OK) {
            printf("补光已点亮（按 NVS light_level）\n");
        } else {
            printf("点亮失败：%s\n", esp_err_to_name(err));
        }
        return;
    }
    if (argc >= 2 && strcmp(argv[1], "off") == 0) {
        esp_err_t err = app_light_off();
        if (err == ESP_OK) {
            printf("补光已熄灭\n");
        } else {
            printf("熄灭失败：%s\n", esp_err_to_name(err));
        }
        return;
    }
    if (argc >= 3 && strcmp(argv[1], "level") == 0) {
        int v = atoi(argv[2]);
        if (v < 0 || v > 255) {
            printf("亮度越界（0..255）\n");
            return;
        }
        esp_err_t err = app_light_set_level((uint8_t)v);
        if (err == ESP_OK) {
            printf("亮度已设为 %d（当场生效，不写 NVS）\n", v);
        } else {
            printf("设置失败：%s\n", esp_err_to_name(err));
        }
        return;
    }
    if (argc >= 2 && strcmp(argv[1], "pulse") == 0) {
        int ms = (argc >= 3) ? atoi(argv[2]) : 500;
        if (ms < 10) {
            ms = 10;
        } else if (ms > 10000) {
            ms = 10000;
        }
        printf("试灯：亮 %d ms…\n", ms);
        esp_err_t err = app_light_pulse((uint16_t)ms);
        if (err == ESP_OK) {
            printf("试灯结束（已熄灭）\n");
        } else {
            printf("试灯失败：%s\n", esp_err_to_name(err));
        }
        return;
    }
    if (argc >= 2 && strcmp(argv[1], "exp") == 0) {
        int v = (argc >= 3) ? atoi(argv[2]) : 0;
        if (v < 0 || v > 255) {
            printf("取值越界（0..255）\n");
            return;
        }
        esp_err_t err = app_light_expander_pwm((uint8_t)v);
        if (err == ESP_OK) {
            printf("已写扩展芯片 PWM=%d（万用表量 LCD 座 pin2 / LCD_BL）\n", v);
        } else {
            printf("写入失败：%s\n", esp_err_to_name(err));
        }
        return;
    }

    const app_cfg_t *c = app_config_get();
    printf("补光通路 : %s（GPIO%u，LEDC 20kHz/10bit）\n",
           app_light_ready() ? "就绪" : "未初始化", (unsigned)c->light_gpio);
    printf("总开关   : light_en=%u\n", (unsigned)c->light_en);
    printf("模式     : %s（light_mode=%u）\n",
           c->light_mode ? "会话常亮" : "拍照脉冲", (unsigned)c->light_mode);
    printf("亮度     : 当前生效=%u / NVS light_level=%u\n",
           (unsigned)app_light_level(), (unsigned)c->light_level);
    printf("AE 等待  : light_lead_ms=%u\n", (unsigned)c->light_lead_ms);
    printf("用法     : light on|off|level <0-255>|pulse [ms]|exp <0-255>\n"
           "  接线：LCD 18pin 座 pin4=GPIO%u → MOS 栅极；pin1=3V3 / pin3=GND 供电\n"
           "  启用：cfg set light_en 1（拍照自动补光）\n", (unsigned)c->light_gpio);
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
    } else if (strcmp(argv[0], "cam") == 0) {
        cmd_cam(argc, argv);
    } else if (strcmp(argv[0], "light") == 0) {
        cmd_light(argc, argv);
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
