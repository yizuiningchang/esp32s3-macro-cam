# 第三方组件补丁说明

## cam_hal.c.patched

来源：`managed_components/espressif__esp32-camera/driver/cam_hal.c`（esp32-camera 2.1.7）
内容：**NO-EOI 丢帧修复**——极暗场景下 OV5640 输出的 JPEG 极短（可低至数 KB），
真正的 JPEG 结束标记（EOI）落在最后一个 DMA 节点之前，原版驱动的"尾部窗口搜索"
找不到它 → 报 "NO-EOI" 丢帧 → `esp_camera_fb_get()` 4 秒超时返回 NULL → **照片存不下来**
（实测：全遮光拍摄必现）。补丁把 EOI 探测窗口放宽为"整个已接收长度"，语义不变
（前向搜索取最早的 EOI，本帧 JPEG 从偏移 0 开始，第一个 EOI 即本帧 EOI）。

随包提供的 `managed_components/` 已包含此补丁（文件内注释含 `[esp32s3_macro_cam 补丁]` 标记）。
**仅当组件被重新下载/覆盖导致补丁丢失时**，用本文件覆盖回去：

```
cp tools/patches/cam_hal.c.patched managed_components/espressif__esp32-camera/driver/cam_hal.c
```

校验补丁在不在：

```
grep -c "esp32s3_macro_cam 补丁" managed_components/espressif__esp32-camera/driver/cam_hal.c
# 输出 ≥1 说明补丁在；0 说明已被覆盖，需重新打上
```

（同目录下的 `cam_hal.c.official-backup` 为改补丁前的原厂副本，可作 diff 基准。）
