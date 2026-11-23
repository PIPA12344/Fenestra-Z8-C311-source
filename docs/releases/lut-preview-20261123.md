# LUT 预览首次公开发布

2026 年 11 月 23 日 · Nikon Z 8 C3.11 · Fenestra `cold-retry3`

### 更新内容

- **取景与 JPEG 使用同一份 LUT。** 在「照片拍摄菜单 → 选择 LUT」选取存储卡上的 `.cube`。载入后可在后屏或电子取景器查看色彩，拍摄受支持尺寸的 JPEG 时，主图和内嵌预览会一起应用所选 LUT。
- **按分类找 LUT。** 文件可直接放进 `FENESTRA/`，也可放在 `FENESTRA/LUTS/<分类>/`。每页显示最多 20 个条目。修复分类页面在载入和翻页时丢失文件名的问题，按方向键的左键或触摸返回可回到原分类位置。
- **记住上次选择。** 菜单显示读取、处理和载入结果。插件将选择保存到当前卡，冷启动后可恢复。要关闭效果，在分类列表选择「无LUT」。

### 安装

将 `lut_preview.wlm` 复制到启动卡的 `FENESTRA/`，放入自己的 `.cube`，完整冷启动后进入照片拍摄菜单。已装旧版独立 `jpeg_lut.wlm` 的用户，请先将旧文件移出插件目录，避免重复加载 JPEG 组件。[下载插件](https://github.com/PIPA12344/Fenestra-Z8-C311-source/releases/download/wlm-2026.11.23-lut-preview/lut_preview.wlm) · [存储卡目录示例与操作说明](https://github.com/PIPA12344/Fenestra-Z8-C311-source/blob/wlm-2026.11.23-lut-preview/docs/plugins/lut_preview.md)

### 验证

用户已在 Nikon Z 8 真机复测，确认插件可用。公开源码重建的 WLM 与开发版本逐字节一致。离线回归覆盖分类翻页与返回、已选文件恢复、JPEG 与显示预览的颜色处理，以及与正式版照片工具 r10-c1 的组合加载。[从源码构建](https://github.com/PIPA12344/Fenestra-Z8-C311-source/blob/wlm-2026.11.23-lut-preview/docs/z8-build.zh-CN.md#lut-预览)

`lut_preview.wlm` 为 **114,302 字节**。SHA-256 为 `f2c134b64e6ee1e1467b7ed272bd5e4ae1dbb7642195d156f9db97f57237ba50`。
