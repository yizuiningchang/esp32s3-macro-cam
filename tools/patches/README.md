# 第三方组件补丁

## cam_hal.c.patched

**来源**：`managed_components/espressif__esp32-camera/driver/cam_hal.c`（esp32-camera **2.1.7**）
**许可**：Apache-2.0（沿用上游）

### 修复的问题

极暗场景（如全遮光）下 OV5640 输出的 JPEG 极短，可低至数 KB。原版驱动只在**最后一个 DMA 节点**内搜索 JPEG 结束标记（EOI），而短帧的 EOI 可能落在更早的节点里：

```
NO-EOI 丢帧 → esp_camera_fb_get() 4 秒超时返回 NULL → 照片存不下来
```

实测全遮光拍摄**必现**。

### 补丁内容

把 EOI 探测窗口从「最后一个节点」放宽为「整个已接收长度」。语义不变：从前向后搜索取最早的 EOI，本帧 JPEG 从偏移 0 开始，第一个 EOI 即本帧 EOI。

### 应用方式

仓库**不包含** `managed_components/`，组件由管理器按 `dependencies.lock` 下载，因此补丁需在首次编译后手动应用：

```bash
idf.py build                      # 触发组件下载
python tools/apply_patch.py       # 幂等：已打过则跳过
idf.py build                      # 增量重编
```

### 校验

```bash
grep -c "esp32s3_macro_cam 补丁" \
  managed_components/espressif__esp32-camera/driver/cam_hal.c
# >=1 补丁在；0 被覆盖了
```

`apply_patch.py` 首次应用时会把原厂文件备份为 `cam_hal.c.orig`，可作 diff 基准。

### 上游状态

该问题在 esp32-camera 后续版本是否修复尚未确认。升级组件版本前需重新评估补丁是否仍必要，并复核 `dependencies.lock` 的 `component_hash`。
