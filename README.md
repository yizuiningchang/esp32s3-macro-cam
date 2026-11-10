# ESP32-S3 微距拍摄终端

面向近距离表面成像的低功耗手持终端固件。核心流程：**按键唤醒 → 拍照 → 存 TF 卡 → 声光反馈 → 自动深睡**。

为「每天几十张、一次充电用两个月」的现场采集场景设计：常态深睡、按键即拍、无需联网、无需交互界面。

---

## 设计指标

| 项 | 目标 | 实测 |
|---|---|---|
| 成像分辨率 | 0.2 mm 特征 @ 100 mm 工作距离 ≥ 3 px | 3.6 px（UXGA 1600×1200） |
| 单张体积 | ≤ 500 KB | ≈ 57 KB（UXGA / Q12） |
| 按键 → 落卡 | ≤ 3 s | ≈ 0.82 s（会话内第 2 张起） |
| 深睡唤醒 → 落卡 | ≤ 5 s | 未实测 |
| 深睡电流 | ≤ 20 µA | **未实测**（见 [已知问题](docs/DESIGN.md#5-已知问题与权衡)） |
| 续航 | ≥ 60 天（24~48 张/天） | 依赖深睡电流，同上 |

---

## 硬件平台

微雪 **ESP32-S3-CAM-OV5640** 开发板。

| 部件 | 规格 |
|---|---|
| 主控 | ESP32-S3R8，双核 240 MHz，8 MB PSRAM（Octal 80 MHz），16 MB Flash |
| 摄像头 | OV5640，定焦，I2C 地址 `0x3C`，XCLK 20 MHz，DVP 接口 |
| 存储 | TF 卡，SDMMC **1-bit**（CLK=GPIO16 / CMD=GPIO43 / D0=GPIO44） |
| 音频 | ES8311 codec + NS4150B 功放（BTL），I2S 16 kHz |
| IO 扩展 | CH32V003（I2C `0x24`）：EXIO3=摄像头 PWDN，EXIO4=功放使能，EXIO6=状态 LED |
| 按键 | PWR = **GPIO15**（RTC ext1，低有效，10 kΩ 上拉） |
| 串口 | 原生 USB-Serial-JTAG（无桥片），主控台与烧录共用同一 USB-C 口 |

> ⚠️ PWR 键在 ESP32-S3 上不是 strapping 引脚，可安全用作唤醒源。**不要改用 GPIO0 / BOOT**——它是 strapping 脚，按下会直接复位进下载模式。

---

## 快速开始

```bash
git clone https://github.com/yizuiningchang/esp32s3-macro-cam.git
cd esp32s3-macro-cam
idf.py build
idf.py -p <PORT> flash monitor
```

- ESP-IDF **v5.5.x**（`idf_component.yml` 要求 `>=5.5`），不要用 6.x
- 退出 monitor：`Ctrl + ]`
- 首次编译会按 `dependencies.lock` 拉取托管组件，需要联网

**正常启动的关键日志**：

```
cfg: 配置载入完成：quality=12 frame_size=13 af_settle=0ms ... hold_awake=300s
camera: 摄像头就绪：PID=0x5640 分辨率=13 质量=12
main: 进入唤醒会话：保持 300 s
```

完整环境要求与踩坑清单见 **[docs/DEPLOYMENT.md](docs/DEPLOYMENT.md)**。

---

## 操作方式

### 按键（PWR = GPIO15）

| 操作 | 行为 |
|---|---|
| 深睡中短按 | 仅唤醒，不拍照 |
| 会话中短按 | 拍照 + 存卡 + 声音反馈 |
| 长按 > 1.5 s | 立即休眠 |
| 无操作满 300 s | 自动休眠（提前 10 s 播提示音） |

### 串口命令

```
help                 命令列表
status               电量 / TF 卡 / 拍摄计数 / 会话剩余时间 / 补光状态
snap                 立即拍照一次
cam show             回读传感器实际生效参数与 AE 落点
cam apply            NVS 参数下发到传感器
cfg list             列出当前配置
cfg set <键> <值>     修改配置（写 NVS，掉电保留）
tone <n>             试听提示音
light on|off|pulse   补光测试（需接外置灯板）
sleep                立即入睡
btn [秒]             监听按键电平
reset                恢复出厂默认（需二次确认）
```

> `cfg set` 只写 NVS，**必须紧跟 `cam apply` 才会下发到传感器**。掉电重启后固件会在摄像头初始化末尾自动 apply，无需人工干预。

---

## 画质调参

所有画质参数可在运行时通过串口调整，无需重新编译：

| 参数 | 默认 | 范围 | 说明 |
|---|---|---|---|
| `frame_size` | 13（HD 1280×720） | 见 `sensor.h` | 枚举值随 esp32-camera 版本变化，**勿硬编码数字** |
| `jpeg_quality` | 12 | 0~63 | 越小画质越好、体积越大 |
| `gain_ceil` | 128（8×） | 0~248 | AGC 增益上限。压低可抑制暗场竖纹 |
| `denoise` | 8 | 0~255 | OV5640 降噪档，暗场收益显著 |
| `sharpness` | 5 | 0~255 | ISP 锐度（中性值 3） |
| `ae_level` | 255（不干预） | -2~2 映射 | AE 目标亮度偏移 |
| `aec_limit` | 不干预（984 行） | — | 最大曝光行数 |
| `light_en` | 0（关） | 0/1 | 外置补光灯 |

**暗场竖纹的成因与处置**：低照度下 AE 会把模拟增益拉到出厂上限（≈15.5×），列向读出噪声随之放大成竖条。压低 `gain_ceil` 到 8× + 开降噪是实测有效的组合，但**治本手段是补光**。完整机理与实验数据见 [docs/DESIGN.md](docs/DESIGN.md)。

---

## 项目结构

```
esp32s3-macro-cam/
├─ main/
│   ├─ main.c              主流程：唤醒会话 / 拍照 / 自动休眠
│   ├─ app_camera.*        摄像头初始化、抓帧、运行时调参
│   ├─ app_config.*        NVS 参数层（分组存储 + 基线版本迁移）
│   ├─ app_cmd.*           串口命令行
│   ├─ app_light.*         可选外置补光（LEDC PWM）
│   ├─ app_audio.*         提示音播放
│   ├─ app_led.*           状态 LED
│   ├─ app_power.*         电源管理、深睡降流、唤醒判定
│   ├─ app_session.*       统一计时（会话超时 / 保持唤醒）
│   └─ app_storage.*       TF 卡挂载、JPEG 落盘、序号防覆盖
├─ tools/                  串口与固件校验脚本、提示音生成、补丁备份
├─ docs/                   设计说明 / 部署指南 / 验证清单
├─ sdkconfig               实测通过的工程配置
├─ sdkconfig.defaults      默认值模板
├─ partitions.csv          分区表
└─ dependencies.lock       托管组件版本锁定
```

---

## 文档

| 文档 | 内容 |
|---|---|
| [docs/DESIGN.md](docs/DESIGN.md) | 系统架构、关键设计决策、已知问题与权衡 |
| [docs/DEPLOYMENT.md](docs/DEPLOYMENT.md) | 环境搭建、烧录、参数迁移、常见问题 |
| [docs/VERIFICATION.md](docs/VERIFICATION.md) | 发布前验证清单 |

---

## 已知限制

- **定焦模组近距失焦**：出厂对焦点约 1.5 m，100 mm 工作距离下弥散圆约 11 px，是本项目画质的首要瓶颈。属光学硬件问题，软件无法补偿。
- **深睡电流未实测**：缺测量工具，`≤20 µA` 为目标值而非实测值。
- **电量阈值未标定**：`app_power_battery_low()` 当前恒返回 false。
- **摄像头 PWDN 极性未标定**：现按高有效处理，需深睡电流实测确认。

---

## 许可

[Apache-2.0](LICENSE)。`tools/patches/` 下的 `esp32-camera` 补丁沿用上游 Apache-2.0 许可，详见该目录说明。
