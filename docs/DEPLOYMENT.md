# 部署指南

> 面向克隆本仓库、希望自行编译烧录的开发者。
> 设计原理见 [DESIGN.md](DESIGN.md)，发布验证见 [VERIFICATION.md](VERIFICATION.md)。

---

## 1. 环境要求

| 项 | 要求 |
|---|---|
| ESP-IDF | **v5.5.x**（推荐 5.5.1），最低 5.5（见 `main/idf_component.yml`） |
| 目标芯片 | `esp32s3` |
| 操作系统 | Windows / Linux / macOS（本项目在 Windows 上开发验证） |
| 硬件 | 微雪 ESP32-S3-CAM-OV5640 板 + TF 卡（FAT32）+ USB-C 数据线 |
| 网络 | **首次编译需要联网**，用于拉取托管组件 |

> 本板没有 USB 转串口桥片，串口就是 ESP32-S3 原生 USB-Serial-JTAG，Windows 10/11 免驱。烧录与看日志共用同一个 USB-C 口。

---

## 2. 快速开始

```bash
# 1) 克隆
git clone https://github.com/yizuiningchang/esp32s3-macro-cam.git
cd esp32s3-macro-cam

# 2) 首次编译（拉取托管组件，约 3~6 分钟）
idf.py build

# 3) 应用摄像头补丁（关键！否则暗场拍照会丢帧）
python tools/apply_patch.py

# 4) 增量重编
idf.py build

# 5) 烧录 + 监视
idf.py -p <PORT> flash monitor
```

`<PORT>`：Windows 形如 `COM5`，Linux 形如 `/dev/ttyACM0`。退出 monitor 用 `Ctrl + ]`。

**为什么必须打补丁**：仓库不包含 `managed_components/`，组件由管理器下载的是官方原版。官方 2.1.7 在极暗场景会因 NO-EOI 超时丢帧，详见 [`tools/patches/README.md`](../tools/patches/README.md)。

**正常启动的关键日志**：

```
cfg: 配置载入完成：quality=12 frame_size=13 af_settle=0ms ... hold_awake=300s
camera: 摄像头就绪：PID=0x5640 分辨率=13 质量=12
main: 进入唤醒会话：保持 300 s
```

---

## 3. 五个必读的坑

### 3.1 深睡会切断 USB 串口

设备进入深睡后 USB-Serial-JTAG 失电，主机侧串口直接消失。这是**设计预期**，不是故障。

- 按 PWR 键唤醒即可恢复
- `esptool` 连不上串口 = 设备在深睡
- 唤醒后有 `hold_awake_s`（默认 300 s）的维护窗口

### 3.2 复位 ≠ 上电

esptool 烧录会话之后，芯片可能滞留在 ROM 下载模式（日志特征 `boot:0x0 (DOWNLOAD)` + `waiting for download`）。此时软复位无效。

**唯一可靠恢复：拔插 USB 线**（上电复位），全程不要按 BOOT。

判别：`esptool --before no_reset read_mac` 能一次握手成功 ⇒ 芯片在下载模式，需要拔插。

### 3.3 打开串口会复位芯片

`pyserial` 打开串口这个动作本身会复位 ESP32-S3。所以「不复位只读」也能拿到完整启动日志；IDE 的 Monitor 打开即复位，属正常行为。

### 3.4 TF 卡可能卡死（0x107），只能断电恢复

现象：`sdmmc_init_ocr: send_op_cond (1) returned 0x107`（超时），重试 3 次仍失败。

恢复：**拔插 USB 线**或重新插拔 TF 卡——只有物理断电能让卡复位。

预防：避免频繁硬复位；固件在进入深睡前会自动卸载卡。

### 3.5 参数自动迁移

所有画质参数存在 NVS（命名空间 `crackcam`，历史遗留名称，勿改）。固件内置参数基线版本机制：

- 启动时发现版本落后 → **自动把画质类参数重置为编译期默认值**
- **不动**现场数据（文件序号、会话时长、音量、补光配置）
- 日志特征：`参数基线版本 0 → 1：自动重置画质类参数为编译期默认值`

**因此：无论板子以前跑过什么版本，直接烧录即可。** 只有需要「连现场数据一起恢复出厂」时才手工清 NVS：

```bash
python -m esptool --chip esp32s3 -p <PORT> erase_region 0x9000 0x6000
```

⚠️ 顺序必须是**先烧录 → 后清 NVS → 再复位**。反了的话芯片会用旧固件启动，把旧默认值又写回去。

---

## 4. 设备操作

### 按键（PWR = GPIO15）

| 操作 | 行为 |
|---|---|
| 深睡中短按 | 仅唤醒，不拍照 |
| 会话中短按 | 拍照 + 存卡 + 声音反馈 |
| 长按 > 1.5 s | 立即休眠 |
| 无操作满 300 s | 自动休眠（提前 10 s 提示） |

### 串口命令

```
help                 命令列表
status               电量 / TF 卡 / 拍摄计数 / 会话剩余 / 补光状态
snap                 立即拍照
cam show             回读传感器实际参数与 AE 落点
cam apply            NVS 参数下发到传感器
cfg list             列出当前配置
cfg set <键> <值>     修改配置（写 NVS）
tone <n>             试听提示音
light on|off|pulse   补光测试
sleep                立即入睡
btn [秒]             监听按键电平
reset                恢复出厂默认（需二次确认）
```

### 画质基线

| 参数 | 值 | 说明 |
|---|---|---|
| `frame_size` | 13 = HD 1280×720 | binning 档，抓帧快，单张约 23 KB |
| `jpeg_quality` | 12 | 越小越好 |
| `gain_ceil` | 128（8×） | AGC 上限，压暗场竖纹的主要手段 |
| `denoise` | 8 | 暗场收益显著 |
| `sharpness` | 5 | ISP 锐度 +2（中性 3） |
| `aec_limit` | 不干预（出厂 984 行） | 最大曝光行数 |
| `light_en` | 0 | 外置补光 |

**为什么是这些值**：低照度下 AE 会把模拟增益拉到出厂上限（≈15.5×），列向读出噪声被放大成竖纹。压增益 + 开降噪 + 提锐度是实测定下的组合。**治本手段是补光**，参数压制只是缓解。

---

## 5. 目录结构

```
esp32s3-macro-cam/
├─ main/                  应用源码
├─ tools/
│   ├─ apply_patch.py     ★ 应用摄像头补丁（首次编译后执行）
│   ├─ patches/           补丁文件与说明
│   ├─ send_cmd.py        串口发命令
│   ├─ serial_capture.py  抓串口日志
│   ├─ wait_boot.py       守候上电第一刻日志
│   ├─ gen_tones.py       重新生成提示音
│   ├─ analyze_backup.py  固件整片备份分析
│   └─ verify_backup.py   固件备份校验
├─ docs/                  设计 / 部署 / 验证文档
├─ managed_components/    (编译时生成，不在版本库中)
├─ sdkconfig              实测通过的配置（勿手改，走 menuconfig）
├─ sdkconfig.defaults     默认值模板
├─ partitions.csv         分区表
└─ dependencies.lock      组件版本锁定
```

---

## 6. 可选工具

需要 Python 3.11+：

```bash
pip install pyserial           # 串口类脚本
pip install pillow numpy       # analyze_backup.py 的图像分析部分
```

| 脚本 | 用途 | 示例 |
|---|---|---|
| `send_cmd.py` | 发命令并回显 | `python send_cmd.py COM5 "cfg list"` |
| `serial_capture.py` | 抓日志（内置复位时序） | `python serial_capture.py COM5 15 out.log` |
| `wait_boot.py` | 守候「上电第一刻」 | 见脚本头注释 |
| `gen_tones.py` | 重新生成 `app_tones.h` | `python gen_tones.py` |
| `check_app_img.py` | 解析 app 镜像结构与校验和 | `python check_app_img.py <bin>` |
| `check_otadata.py` | 解析 otadata，判断启动分区 | `python check_otadata.py <backup.bin>` |

---

## 7. 常见问题

| 症状 | 处理 |
|---|---|
| 编译报组件下载失败 | 检查网络；确认 `dependencies.lock` 完整 |
| 暗场拍照存不下来 | 补丁没打，执行 `python tools/apply_patch.py` 后重编 |
| 串口列表没有设备 | 设备在深睡（按 PWR）；或换一根**数据线**（非充电线） |
| 拍照失败（三短音 + LED 闪 3 次） | 看日志归因：卡缺失 / 容量不足 / 摄像头异常 / 写卡失败 |
| 暗场竖纹重 | 提高照度；或 `cfg set gain_ceil 64` + `cam apply` |
| 照片偏软 | 定焦模组对焦点在远处，100 mm 近距失焦，属硬件限制 |
| 恢复出厂画质 | `cfg set gain_ceil 248` / `denoise 255` / `sharpness 255`，再 `cam apply` |
| monitor 无输出 | 确认打开的是 USB 口；设备需处于唤醒态 |
| 反复进下载模式 | 拔插 USB |

---

## 8. 部署自检

- [ ] `idf.py build` 通过（0 error）
- [ ] `python tools/apply_patch.py` 输出「补丁应用成功」
- [ ] 烧录后启动日志出现 `摄像头就绪：PID=0x5640`
- [ ] `cfg list` / `cam show` 回读：`frame_size=13`、`gain_ceil=128`、`denoise=8`
- [ ] 按 PWR 拍一张，TF 卡 `DCIM/` 出现 `IMG_xxxxxx.jpg`
- [ ] 全遮光拍一张，确认不报 NO-EOI
- [ ] 静置至超时，串口消失；按 PWR 恢复
