/**
 * @file  app_camera.c
 * @brief 摄像头子系统实现
 */
#include "app_camera.h"

#include "app_config.h"
#include "bsp/esp-bsp.h"
#include "esp_io_expander.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <stdio.h>   /* app_camera_dump_tuning 用 printf */

static const char *TAG = "camera";

/* ---------------- v1.8 画质调参寄存器（OV5640，暗场竖纹治理）----------------
 * 竖纹是**逐帧随机、竖向短程相关(8~32px)的读出噪声**，实测幅度 ∝ 模拟增益。
 * 因此第一抓手是**压低模拟增益上限**，而不是（也无法靠）任何数字滤波根治。
 * 下表寄存器含义见 OV5640 datasheet（已与 ov5640_settings.h 出厂值交叉核对）。 */
#define OV5640_REG_AGC_MAX_H    0x3A18   /* AGC 最大增益 高位[1:0] */
#define OV5640_REG_AGC_MAX_L    0x3A19   /* AGC 最大增益 低位[7:0]；增益 = 码/16 */
#define OV5640_REG_AEC_MAX_H    0x3A0D   /* AEC 最大曝光 高位 */
#define OV5640_REG_AEC_MAX_L    0x3A0E   /* AEC 最大曝光 低位 */
#define OV5640_REG_AE_TARGET_H  0x3A0F   /* AE 目标亮度 高位 */
#define OV5640_REG_AE_TARGET_L  0x3A10   /* AE 目标亮度 低位 */
#define OV5640_REG_AEC_H        0x3500   /* 当前曝光 高[3:0] */
#define OV5640_REG_AEC_M        0x3501   /* 当前曝光 中 */
#define OV5640_REG_AEC_L        0x3502   /* 当前曝光 低 */
#define OV5640_REG_AGC_H        0x350A   /* 当前增益 高[1:0] */
#define OV5640_REG_AGC_L        0x350B   /* 当前增益 低[7:0]；增益 = 码/16 */
#define OV5640_REG_GAIN_CEIL_DEFAULT  248  /* 出厂值 0x3A19=0xF8 → ≈15.5x */

/* v1.9 新增（暗场竖纹治理·曝光侧）--------------------------------------------
 * 出厂表 {0x3A02,0x03}{0x3A03,0xD8} = AEC 曝光上限 984 行（半帧）。
 * UXGA(VTS=1968行)下 AE 曝光用尽后只能拉增益 → 列噪声被放大成竖纹。
 * 抬高此上限让 AE「用曝光换增益」。
 * ⚠ 只对不 binning 的大帧有意义：4:3 binning 档 VTS=984、HD(16:9) VTS=744，
 *   出厂 984 行上限已够甚至超额，那些档无需设置本项。 */
#define OV5640_REG_AEC_LIM_H    0x3A02   /* AEC 曝光上限 高位[3:0] */
#define OV5640_REG_AEC_LIM_L    0x3A03   /* AEC 曝光上限 低位[7:0]；单位=行 */
#define OV5640_REG_AEC_LIM_DEFAULT  984  /* 出厂值 0x3A02:03 = 0x3D8 = 984 行 */
#define OV5640_REG_VTS_H        0x380E   /* 帧总行数(VTS) 高位[3:0] */
#define OV5640_REG_VTS_L        0x380F   /* 帧总行数(VTS) 低位[7:0] */

/* 哨兵：NVS 里表示"不干预该项"，保持出厂寄存器表值 */
#define TUNE_SKIP_U8   0xFFu
#define TUNE_SKIP_U16  0xFFFFu

/* EXIO3 = 摄像头 PWDN（原理图 PIN OUT 表；��。
 * 官方 BSP 只把 EXIO0/1/2/4/5/6 配为输出，EXIO3 从未被驱动，故其有效电平
 * 在文档中标注为"待实测"。约定 OV5640 PWDN 高有效（=1 关断摄像头）。
 * 阶段 4 实测若深睡电流不降（或摄像头行为异常），把 CAM_PWDN_ACTIVE_LEVEL 改 0 重测。 */
#define EXIO_CAM_PWDN           IO_EXPANDER_PIN_NUM_3
#define CAM_PWDN_ACTIVE_LEVEL   1
#define CAM_PWDN_INACTIVE_LEVEL (1 - CAM_PWDN_ACTIVE_LEVEL)

/* 抓帧前稳定延时：等 XCLK/PLL 稳定 + 大负载错峰 */
#define CAPTURE_SETTLE_MS       15

static bool s_inited = false;
static bool s_first_capture_after_init = true;  /**< init 后首张需丢帧（AE/AWB 未收敛） */

esp_err_t app_camera_power_up(void)
{
    esp_io_expander_handle_t io = bsp_get_io_expander_handle();
    if (io == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    esp_err_t err = esp_io_expander_set_dir(io, EXIO_CAM_PWDN, IO_EXPANDER_OUTPUT);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "设置 EXIO3(PWDN) 方向失败：%s", esp_err_to_name(err));
        return err;
    }
    return esp_io_expander_set_level(io, EXIO_CAM_PWDN, CAM_PWDN_INACTIVE_LEVEL);
}

esp_err_t app_camera_power_down(void)
{
    esp_io_expander_handle_t io = bsp_get_io_expander_handle();
    if (io == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    esp_err_t err = esp_io_expander_set_dir(io, EXIO_CAM_PWDN, IO_EXPANDER_OUTPUT);
    if (err == ESP_OK) {
        err = esp_io_expander_set_level(io, EXIO_CAM_PWDN, CAM_PWDN_ACTIVE_LEVEL);
    }
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "摄像头 PWDN 已断言（EXIO3=%d）", (int)CAM_PWDN_ACTIVE_LEVEL);
    } else {
        ESP_LOGW(TAG, "断言 PWDN 失败：%s", esp_err_to_name(err));
    }
    return err;
}

/** 读寄存器，失败返回 def（get_reg 出错时为负） */
static int rd_reg(sensor_t *s, int reg, int mask, int def)
{
    int v = s->get_reg(s, reg, mask);
    return v < 0 ? def : v;
}

esp_err_t app_camera_apply_tuning(void)
{
    if (!s_inited) {
        ESP_LOGW(TAG, "摄像头未初始化，无法下发画质参数");
        return ESP_ERR_INVALID_STATE;
    }
    sensor_t *s = esp_camera_sensor_get();
    if (s == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    /* 定焦 OV5640 上这些接口均已实现；若为空说明换了 sensor，直接跳过以免空指针 */
    if (s->set_reg == NULL || s->set_ae_level == NULL || s->set_denoise == NULL ||
        s->set_exposure_ctrl == NULL || s->set_gain_ctrl == NULL ||
        s->set_aec_value == NULL || s->set_agc_gain == NULL ||
        s->set_whitebal == NULL || s->set_awb_gain == NULL || s->set_sharpness == NULL) {
        ESP_LOGW(TAG, "传感器缺少调参接口，跳过画质下发");
        return ESP_ERR_NOT_SUPPORTED;
    }

    const app_cfg_t *cfg = app_config_get();

    /* 1) AGC 最大增益上限 —— 压竖纹的第一抓手（竖纹幅度 ∝ 模拟增益）。
     *    直写寄存器，不用 set_gainceiling()：后者把枚举序号当原始寄存器码写
     *    （0x3A18=(l>>8)&3, 0x3A19=l&0xFF），按标准枚举传值只会得到码 0~6。
     *    0x3A18 仅低 2 位有效；增益 = 码/16。 */
    if (cfg->gain_ceil != TUNE_SKIP_U16) {
        uint16_t code = cfg->gain_ceil > 1023 ? 1023 : cfg->gain_ceil;
        int e1 = s->set_reg(s, OV5640_REG_AGC_MAX_H, 0xFF, (code >> 8) & 0x03);
        int e2 = s->set_reg(s, OV5640_REG_AGC_MAX_L, 0xFF, code & 0xFF);
        if (e1 || e2) {
            ESP_LOGE(TAG, "下发 gain_ceil=%u 失败", (unsigned)code);
        } else {
            ESP_LOGI(TAG, "gain_ceil = %u（上限 ≈%.1fx；出厂 %u ≈ %.1fx）",
                     (unsigned)code, code / 16.0,
                     OV5640_REG_GAIN_CEIL_DEFAULT, OV5640_REG_GAIN_CEIL_DEFAULT / 16.0);
        }
    }

    /* 1b) AEC 曝光上限（v1.9）—— 让 AE「用曝光换增益」。
     *     出厂 984 行（半帧）：UXGA 下暗场曝光用尽后 AE 只能狂拉增益 → 竖纹。
     *     抬高上限（如 1960）后暗场曝光可翻倍、增益相应回落。
     *     写入前按当前 VTS（0x380E:0F）钳位，避免超过帧长导致 VSYNC 异常。 */
    if (cfg->aec_limit != TUNE_SKIP_U16) {
        int vts_h = rd_reg(s, OV5640_REG_VTS_H, 0xFF, -1);
        int vts_l = rd_reg(s, OV5640_REG_VTS_L, 0xFF, -1);
        int vts = (vts_h >= 0 && vts_l >= 0) ? ((vts_h & 0x0F) << 8) | (vts_l & 0xFF) : 0;
        uint16_t lim = cfg->aec_limit;
        if (vts > 0 && (int)lim > vts - 8) {
            lim = (uint16_t)(vts - 8);
            ESP_LOGW(TAG, "aec_limit 按当前 VTS=%d 钳位到 %u 行", vts, (unsigned)lim);
        }
        int e1 = s->set_reg(s, OV5640_REG_AEC_LIM_H, 0xFF, (lim >> 8) & 0x0F);
        int e2 = s->set_reg(s, OV5640_REG_AEC_LIM_L, 0xFF, lim & 0xFF);
        if (e1 || e2) {
            ESP_LOGE(TAG, "下发 aec_limit=%u 失败", (unsigned)lim);
        } else {
            ESP_LOGI(TAG, "aec_limit = %u 行（出厂 %u 行；当前 VTS=%d 行，暗场曝光余量↑ → AE 拉增益的压力↓）",
                     (unsigned)lim, OV5640_REG_AEC_LIM_DEFAULT, vts);
        }
    }

    /* 2) AE 目标偏移：NVS 0..10 → 驱动 -5..+5（5 = 中性）。
     *    调低 → AE 目标更暗 → 模拟增益更低 → 竖纹更弱（代价：整体变暗）。 */
    if (cfg->ae_level != TUNE_SKIP_U8) {
        int lvl = (int)cfg->ae_level - 5;
        if (s->set_ae_level(s, lvl) == 0) {
            ESP_LOGI(TAG, "ae_level = %d（AE 目标码 %d）", lvl, (lvl + 5) * 10 + 5);
        } else {
            ESP_LOGE(TAG, "下发 ae_level=%d 失败（有效 -5..+5）", lvl);
        }
    }

    /* 3) 降噪档 0..8（0 = 关），针对短程相关噪声 */
    if (cfg->denoise != TUNE_SKIP_U8) {
        if (s->set_denoise(s, (int)cfg->denoise) == 0) {
            ESP_LOGI(TAG, "denoise = %u", (unsigned)cfg->denoise);
        } else {
            ESP_LOGE(TAG, "下发 denoise=%u 失败（有效 0..8）", (unsigned)cfg->denoise);
        }
    }

    /* 4) 曝光 / 增益：锁定或自动（锁定时灰度不再随场景漂移，便于定量比对） */
    if (cfg->ae_lock) {
        s->set_exposure_ctrl(s, 0);
        s->set_gain_ctrl(s, 0);
        s->set_aec_value(s, (int)cfg->aec_value);
        s->set_agc_gain(s, (int)cfg->agc_gain);
        ESP_LOGI(TAG, "AE 已锁定：曝光 %u 行 / 增益档 %u",
                 (unsigned)cfg->aec_value, (unsigned)cfg->agc_gain);
    } else {
        s->set_exposure_ctrl(s, 1);
        s->set_gain_ctrl(s, 1);
        ESP_LOGI(TAG, "AE 保持自动");
    }

    /* 5) 白平衡：锁定或自动 */
    if (cfg->wb_lock) {
        s->set_whitebal(s, 0);
        s->set_awb_gain(s, 0);
        ESP_LOGI(TAG, "AWB 已锁定");
    } else {
        s->set_whitebal(s, 1);
        s->set_awb_gain(s, 1);
        ESP_LOGI(TAG, "AWB 保持自动");
    }

    /* 6) ISP 锐度（v1.9）：NVS 0..6 → 驱动 -3..+3（3 = 中性）。
     *    ⚠ 只能改善边缘对比度观感，不能恢复失焦丢失的细节；
     *    下发会关闭 ISP 自动锐化（0x5308 bit6 清 0）切固定档。 */
    if (cfg->sharpness != TUNE_SKIP_U8) {
        int lvl = (int)cfg->sharpness - 3;
        if (s->set_sharpness(s, lvl) == 0) {
            ESP_LOGI(TAG, "sharpness = %d（ISP 固定档，-3..+3）", lvl);
        } else {
            ESP_LOGE(TAG, "下发 sharpness=%d 失败（有效 -3..+3）", lvl);
        }
    }

    return ESP_OK;
}

void app_camera_dump_tuning(void)
{
    if (!s_inited) {
        printf("摄像头未初始化\n");
        return;
    }
    sensor_t *s = esp_camera_sensor_get();
    if (s == NULL) {
        printf("sensor 句柄为空\n");
        return;
    }

    /* 全部从传感器**回读**，确认参数真的落到寄存器上（而不是只写进了 NVS） */
    int gain_ceil = ((rd_reg(s, OV5640_REG_AGC_MAX_H, 0xFF, -1) & 0x03) << 8)
                  | (rd_reg(s, OV5640_REG_AGC_MAX_L, 0xFF, -1) & 0xFF);
    int ae_target = (rd_reg(s, OV5640_REG_AE_TARGET_H, 0xFF, -1) << 8)
                  | (rd_reg(s, OV5640_REG_AE_TARGET_L, 0xFF, -1) & 0xFF);
    /* v1.9 修正：0x3A0D/0E 是防频闪的每帧 band 数上限（60/50Hz），不是 AEC 曝光上限；
     * 真实的 AEC 曝光上限在 0x3A02:03（单位=行），帧总行数 VTS 在 0x380E:0F。 */
    int aec_lim   = ((rd_reg(s, OV5640_REG_AEC_LIM_H, 0xFF, -1) & 0x0F) << 8)
                  | (rd_reg(s, OV5640_REG_AEC_LIM_L, 0xFF, -1) & 0xFF);
    int vts       = ((rd_reg(s, OV5640_REG_VTS_H, 0xFF, -1) & 0x0F) << 8)
                  | (rd_reg(s, OV5640_REG_VTS_L, 0xFF, -1) & 0xFF);
    int band_60   = rd_reg(s, OV5640_REG_AEC_MAX_H, 0xFF, -1);   /* 0x3A0D */
    int band_50   = rd_reg(s, OV5640_REG_AEC_MAX_L, 0xFF, -1);   /* 0x3A0E */
    /* v1.9 修正：0x3500-02 是寄存器码 = 行数×16，÷16 才是曝光行数 */
    int aec_code  = ((rd_reg(s, OV5640_REG_AEC_H, 0xFF, 0) & 0x0F) << 16)
                  | (rd_reg(s, OV5640_REG_AEC_M, 0xFF, 0) << 8)
                  | (rd_reg(s, OV5640_REG_AEC_L, 0xFF, 0));
    int agc_now   = ((rd_reg(s, OV5640_REG_AGC_H, 0xFF, 0) & 0x03) << 8)
                  | (rd_reg(s, OV5640_REG_AGC_L, 0xFF, 0));

    printf("--- 画质参数（从传感器回读）---\n");
    printf("AGC 上限(0x3A18:19) : %d ≈ %.1fx   出厂 %d ≈ %.1fx\n",
           gain_ceil, gain_ceil / 16.0,
           OV5640_REG_GAIN_CEIL_DEFAULT, OV5640_REG_GAIN_CEIL_DEFAULT / 16.0);
    printf("AE 目标(0x3A0F:10)  : %d\n", ae_target);
    printf("AEC 曝光上限(0x3A02:03) : %d 行（出厂 %d 行；防频闪 band 数 60Hz=%d/50Hz=%d）\n",
           aec_lim, OV5640_REG_AEC_LIM_DEFAULT, band_60, band_50);
    printf("帧总行数 VTS(0x380E:0F)  : %d 行\n", vts);
    printf("当前曝光(0x3500-02) : %d 行（寄存器码 %d ÷16）\n", aec_code / 16, aec_code);
    printf("当前增益(0x350A:0B) : %d ≈ %.2fx\n", agc_now, agc_now / 16.0);
    printf("降噪 / 锐度         : %d / %d\n",
           (int)s->status.denoise, (int)s->status.sharpness);
    printf("AEC/AGC/AWB 使能    : %d / %d / %d\n",
           (int)s->status.aec, (int)s->status.agc, (int)s->status.awb);
}

void app_camera_log_ae(void)
{
    if (!s_inited) {
        return;
    }
    sensor_t *s = esp_camera_sensor_get();
    if (s == NULL) {
        return;
    }
    /* 每张照片落卡时打一行 AE 落点（竖纹归因取证：竖纹帧的增益应显著高于干净帧）。
     * 0x3500-02 寄存器码 = 曝光行数×16；0x350A:0B 码 = 增益×16。 */
    int aec_code = ((rd_reg(s, OV5640_REG_AEC_H, 0xFF, 0) & 0x0F) << 16)
                  | (rd_reg(s, OV5640_REG_AEC_M, 0xFF, 0) << 8)
                  | (rd_reg(s, OV5640_REG_AEC_L, 0xFF, 0));
    int agc_now  = ((rd_reg(s, OV5640_REG_AGC_H, 0xFF, 0) & 0x03) << 8)
                  | (rd_reg(s, OV5640_REG_AGC_L, 0xFF, 0));
    ESP_LOGI(TAG, "本帧 AE 落点：曝光 %d 行 / 增益 %.2fx（码 %d）——竖纹帧增益应显著更高",
             aec_code / 16, agc_now / 16.0, agc_now);
}

esp_err_t app_camera_init(void)
{
    if (s_inited) {
        return ESP_OK;
    }

    /* 深睡唤醒后扩展芯片仍保持 PWDN 断言态，必须先解除，否则摄像头不可用 */
    app_camera_power_up();

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
    s_first_capture_after_init = true;   /* 每次重新 init 后，首张恢复丢帧收敛 */

    /* v1.8：把 NVS 里的画质调参下发到传感器（0xFF/0xFFFF = 不干预）。
     * 必须放在 s_inited 置位之后，因为 apply_tuning 会检查该标志。 */
    app_camera_apply_tuning();

    return ESP_OK;
}

camera_fb_t *app_camera_capture(void)
{
    if (!s_inited) {
        ESP_LOGE(TAG, "摄像头未初始化");
        return NULL;
    }

    /* AF 模组到货后此处触发对焦并等待 cfg->af_settle_ms；Phase 1 定焦模组不等待 */
    const app_cfg_t *cfg = app_config_get();
    if (cfg->af_settle_ms > 0) {
        vTaskDelay(pdMS_TO_TICKS(cfg->af_settle_ms));
    }

    /* 抓帧前稳定延时：等 XCLK/PLL 稳，并与其它大负载错峰 */
    vTaskDelay(pdMS_TO_TICKS(CAPTURE_SETTLE_MS));

    /* 丢首帧策略（v1.7）：仅**本次 init 后的第一张**丢弃——AE/AWB 尚未收敛。
     * 后续张不再单独丢帧：快门音（~370 ms 阻塞播放）期间摄像头 DMA 持续出流，
     * 已等效完成"等待最新参数生效"，单独丢帧反而白等一帧（~200 ms）。
     * （快门音在 main.c 的 run_capture_flow 中、调用本函数之前播放。） */
    if (s_first_capture_after_init) {
        s_first_capture_after_init = false;
        camera_fb_t *stale = esp_camera_fb_get();
        if (stale) {
            esp_camera_fb_return(stale);
        }
    }

    camera_fb_t *fb = esp_camera_fb_get();
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

    /* 深睡前的 PWDN 断言由 app_power_prepare_sleep() 显式调用 app_camera_power_down()，
     * 见/ 4.2（EXIO 映射以原理图 PIN OUT 表为准：EXIO3 = CAM PWDN）。 */
    ESP_LOGI(TAG, "摄像头已释放：%s", esp_err_to_name(err));
    return err;
}
