# 发布验证清单

> 发布固件前逐项确认。带 ⚠️ 的项若不符合，设备行为会与设计不符。

---

## 1. 编译期开关

两个调试开关都在 `main/main.c` 顶部，用 `#define` 控制，**发布版必须为 0**。

| 开关 | 开发期 | 发布值 | 不改的后果 |
|---|---|---|---|
| `APP_DEBUG_KEEP_AWAKE` | 1 | **0** | 流程结束不深睡，功耗严重超标，USB 外设长期供电 |
| `APP_CAPTURE_ON_COLD_BOOT` | 1 | **0** | 上电/复位也会拍一张，产生非预期照片 |

两项均为编译期常量，改完必须**全量重编**。

> `KEEP_AWAKE=0` 后设备会进入深睡并切断 USB-Serial-JTAG，主机侧串口消失。这是预期行为。

---

## 2. 功能项

- [ ] **摄像头 PWDN（EXIO3）**：`app_camera_power_down()` 在睡前断言，`app_camera_power_up()` 在唤醒后、init 前解除
      ⚠️ 遗留：极性现按高有效（`CAM_PWDN_ACTIVE_LEVEL 1`），需深睡电流实测确认
- [ ] **音频反馈**：`app_audio.{h,c}` + `app_tones.h`，7 种音效可播，播完关功放（EXIO4=0）
- [ ] **对焦**：Phase 1 为定焦模组，`af_settle_ms` 默认 0（按下即拍）
      换 AF 模组后执行 `cfg set af_settle_ms 500` 启用；改默认值须清 NVS
- [ ] **序号持久化**：挂卡时扫描 `IMG_*.jpg` 最大序号，把 NVS 计数器抬到「最大 + 1」，杜绝清 NVS / 换卡后重号覆盖

---

## 3. 已确认无需处理的现象

### Flash 模式显示不一致

运行时实测 **QIO 80 MHz**（启动日志 `SPI Mode : QIO`），而 `sdkconfig` 里字符串符号 `CONFIG_ESPTOOLPY_FLASHMODE="dio"` 与之不符。

属 Kconfig 符号滞后，**不影响实际行为，不要手改**。要改走 `idf.py menuconfig`。

### SD 卡「CS 脚」不存在

本项目走 SDMMC 1-bit（`D0=GPIO44` / `CMD=GPIO43` / `CLK=GPIO16`）。SDMMC 协议**没有 CS 概念**，BSP 的 `bsp_sdcard_sdspi_mount()` 直接返回「SPI mode is not supported by HW」。

因此「EXIO2 = SD CS」这条对 IDF 路径不成立，BSP 不驱动该脚**不是遗漏**。

---

## 4. 发布执行记录

| 项 | 状态 |
|---|---|
| `APP_CAPTURE_ON_COLD_BOOT` 1 → 0 | ✅ |
| `APP_DEBUG_KEEP_AWAKE` 1 → 0 | ✅ |
| 两开关改动后全量重编 | ✅ |
| 上电/复位不拍照；按键唤醒 → 拍照 → 反馈 → 回深睡 | ✅ 实机验证 |
| 深睡电流实测 | ⏸ **未完成**（无测量工具） |
| EXIO3 PWDN 极性确认 | ⏸ **未完成**（随深睡电流一并确认） |

---

## 5. 会话与休眠行为

| 项 | 值 | 说明 |
|---|---|---|
| `hold_awake_s` | 默认 **300** | 保持唤醒 / 无操作超时；改默认值须清 NVS |
| 自动休眠 | 超时 → 剩余 10 s 播预警 → 静默入睡 | 上电 / 唤醒 / **每次拍照**均重置 |
| 按键交互 | 深睡首按仅唤醒；短按拍照；长按 > 1.5 s 休眠 | 见 [DESIGN.md](DESIGN.md#44-按键语义) |
| 摄像头 | 会话期间常驻，不逐张 deinit | 睡眠前统一 deinit + 断言 PWDN |
| 入睡前刷日志 | `SLEEP_LOG_FLUSH_MS = 200` | 排空 USB-JTAG TX FIFO，避免丢最后几行日志 |

---

## 6. 发布前终检

- [ ] `grep -n "APP_DEBUG_KEEP_AWAKE" main/main.c` 值为 0
- [ ] `grep -n "APP_CAPTURE_ON_COLD_BOOT" main/main.c` 值为 0
- [ ] 全量编译无 warning 级以上的异常
- [ ] `dependencies.lock` 未被意外修改
- [ ] 补丁校验：`grep -c "esp32s3_macro_cam 补丁" managed_components/espressif__esp32-camera/driver/cam_hal.c` ≥ 1
- [ ] 全遮光拍摄可正常落卡（验证 EOI 补丁）
- [ ] 版本号 / 构建时间戳符合预期
