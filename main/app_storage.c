/**
 * @file  app_storage.c
 * @brief TF 卡存储子系统实现
 */
#include "app_storage.h"

#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#include "app_config.h"
#include "bsp/esp-bsp.h"
#include "esp_log.h"
#include "esp_vfs_fat.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "storage";

#define DCIM_DIR       BSP_SD_MOUNT_POINT "/DCIM"
#define PHOTO_PATH_FMT DCIM_DIR "/IMG_%06u.jpg"
#define WRITE_RETRY    2

/** 挂卡重试：TF 卡上电后需要一定稳定时间，接触不良/上电时序偏紧时
 *  首次 CMD1 常报 ESP_ERR_TIMEOUT(0x107)。重试可显著提高一次成功率。
 *  间隔取 50 ms，
 *  既给卡留稳定时间又不显著拉长唤醒总时长。 */
#define MOUNT_RETRY        3
#define MOUNT_RETRY_DELAY  50    /* ms */

static bool s_mounted = false;
static bool s_seq_scanned = false;   /**< 本次上电会话是否已做过序号扫卡对齐 */

/** 解析 "IMG_%u.jpg"（忽略前导零），成功返回 true 并回填序号 */
static bool parse_img_seq(const char *name, uint32_t *out)
{
    if (strncmp(name, "IMG_", 4) != 0) {
        return false;
    }
    const char *p = name + 4;
    if (*p < '0' || *p > '9') {
        return false;
    }
    uint32_t n = 0;
    while (*p >= '0' && *p <= '9') {
        n = n * 10 + (uint32_t)(*p - '0');
        if (n > 99999999u) {
            return false;
        }
        p++;
    }
    if (strcmp(p, ".jpg") != 0) {
        return false;
    }
    *out = n;
    return true;
}

uint32_t app_storage_scan_max_seq(void)
{
    uint32_t max_seq = 0;
    DIR *d = opendir(DCIM_DIR);
    if (d == NULL) {
        return 0;
    }
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        uint32_t n = 0;
        if (parse_img_seq(e->d_name, &n) && n > max_seq) {
            max_seq = n;
        }
    }
    closedir(d);
    return max_seq;
}

esp_err_t app_storage_mount(void)
{
    if (s_mounted) {
        return ESP_OK;
    }

    esp_err_t err = ESP_FAIL;
    for (int attempt = 0; attempt < MOUNT_RETRY; ++attempt) {
        err = bsp_sdcard_mount();
        if (err == ESP_OK) {
            break;
        }
        ESP_LOGW(TAG, "TF 卡挂载失败（第 %d/%d 次）：%s",
                 attempt + 1, MOUNT_RETRY, esp_err_to_name(err));
        if (attempt + 1 < MOUNT_RETRY) {
            vTaskDelay(pdMS_TO_TICKS(MOUNT_RETRY_DELAY));
        }
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "TF 卡挂载失败（已重试 %d 次）：%s", MOUNT_RETRY, esp_err_to_name(err));
        return err;
    }
    s_mounted = true;

    /* 确保 /DCIM 存在 */
    if (mkdir(DCIM_DIR, 0775) != 0 && errno != EEXIST) {
        ESP_LOGW(TAG, "创建目录 %s 失败：errno=%d", DCIM_DIR, errno);
    }

    /* 序号对齐：NVS 被清过/换卡后，计数器可能小于卡上已有最大序号，
     * 若不抬升会覆盖历史照片。这里把 file_seq 抬到"卡上最大序号+1"。
     * v1.7 性能优化：对齐只在**本次上电会话的首次挂卡**执行（遍历 DCIM 实测 ~600 ms）；
     * 会话内连续拍照时 NVS 序号刚对齐过、且每次写卡成功即持久化，无需重复扫卡。
     * 上电/深睡唤醒复位本变量，每个会话自然恢复"首次必扫"的防覆盖语义。 */
    if (!s_seq_scanned) {
        uint32_t card_max = app_storage_scan_max_seq();
        uint32_t cfg_seq = app_config_get()->file_seq;
        if (card_max >= cfg_seq) {
            ESP_LOGI(TAG, "卡上已有最大 IMG_%06u，序号由 NVS(%u) 抬升至 %u 以避免覆盖",
                     (unsigned)card_max, (unsigned)cfg_seq, (unsigned)(card_max + 1));
            app_config_set_file_seq(card_max + 1);
        } else {
            ESP_LOGI(TAG, "卡上最大 IMG_%06u < NVS 序号 %u，沿用 NVS", (unsigned)card_max, (unsigned)cfg_seq);
        }
        s_seq_scanned = true;
    }

    uint64_t total = 0, freeb = 0;
    if (app_storage_get_space(&total, &freeb) == ESP_OK) {
        ESP_LOGI(TAG, "TF 卡已挂载：总 %llu MB，剩余 %llu MB",
                 total / (1024ULL * 1024ULL), freeb / (1024ULL * 1024ULL));
    } else {
        ESP_LOGI(TAG, "TF 卡已挂载");
    }
    return ESP_OK;
}

esp_err_t app_storage_unmount(void)
{
    if (!s_mounted) {
        return ESP_OK;
    }
    esp_err_t err = bsp_sdcard_unmount();
    s_mounted = false;
    return err;
}

bool app_storage_mounted(void)
{
    return s_mounted;
}

esp_err_t app_storage_get_space(uint64_t *total_bytes, uint64_t *free_bytes)
{
    if (!s_mounted) {
        return ESP_ERR_INVALID_STATE;
    }
    uint64_t total = 0, freeb = 0;
    esp_err_t err = esp_vfs_fat_info(BSP_SD_MOUNT_POINT, &total, &freeb);
    if (err != ESP_OK) {
        return err;
    }
    if (total_bytes) {
        *total_bytes = total;
    }
    if (free_bytes) {
        *free_bytes = freeb;
    }
    return ESP_OK;
}

bool app_storage_space_ok(uint8_t low_pct)
{
    uint64_t total = 0, freeb = 0;
    if (app_storage_get_space(&total, &freeb) != ESP_OK || total == 0) {
        return false;
    }
    /* freeb / total < low_pct%  → 停拍 */
    return (freeb * 100ULL) >= (total * (uint64_t)low_pct);
}

static esp_err_t write_file(const char *path, const uint8_t *data, size_t len)
{
    FILE *f = fopen(path, "wb");
    if (f == NULL) {
        ESP_LOGE(TAG, "打开文件失败：%s（errno=%d）", path, errno);
        return ESP_FAIL;
    }

    size_t written = fwrite(data, 1, len, f);
    if (written != len) {
        ESP_LOGE(TAG, "写入不完整：%u/%u 字节", (unsigned)written, (unsigned)len);
        fclose(f);
        remove(path);   /* 删除残件，避免留下坏图 */
        return ESP_FAIL;
    }

    /* 落盘确认后再关闭 */
    if (fclose(f) != 0) {
        ESP_LOGE(TAG, "关闭文件失败：%s", path);
        remove(path);
        return ESP_FAIL;
    }
    return ESP_OK;
}

esp_err_t app_storage_save_jpeg(const uint8_t *data, size_t len, char *out_path, size_t out_path_len)
{
    if (!s_mounted) {
        ESP_LOGE(TAG, "TF 卡未挂载，无法保存");
        return ESP_ERR_INVALID_STATE;
    }
    if (data == NULL || len == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    /* 先取当前序号；仅在写成功后自增并持久化，避免掉电跳号 */
    uint32_t seq = app_config_get()->file_seq;

    char path[64];
    snprintf(path, sizeof(path), PHOTO_PATH_FMT, (unsigned)seq);

    esp_err_t err = ESP_FAIL;
    for (int attempt = 0; attempt <= WRITE_RETRY; ++attempt) {
        err = write_file(path, data, len);
        if (err == ESP_OK) {
            break;
        }
        ESP_LOGW(TAG, "写卡失败，第 %d 次重试", attempt + 1);
    }

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "写卡重试耗尽，放弃本次保存");
        return err;
    }

    /* 写卡成功 → 提交序号 */
    uint32_t used = 0;
    err = app_config_next_file_seq(&used);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "序号持久化失败：%s", esp_err_to_name(err));
    }

    if (out_path && out_path_len > 0) {
        snprintf(out_path, out_path_len, "%s", path);
    }
    ESP_LOGI(TAG, "已保存 %s（%u 字节）", path, (unsigned)len);
    return ESP_OK;
}
