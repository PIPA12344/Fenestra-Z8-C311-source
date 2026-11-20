# 照片工具 r10-c1

2026 年 11 月 20 日正式发布，配套 Nikon Z 8 C3.11 的 `cold-retry3` 底座。

## 更新

机内生成 NEF 时写入 MakerNote `0x008C`，修复高速 RAW 在 Capture One 中无法解码的问题。该字段使用原厂 Z 8 的 578 字节内容。生成头文件时统一计算字段、预览和 RAW 偏移，输入中已有该字段时生成一份有效记录。

用户通过可见 Z8 工作台安装后，确认新拍 NEF 在 Capture One 中兼容。此前测试覆盖 DX C60 和 FX C120，具体 Capture One 版本未记录。FX C120 的原尺寸 NEF 在 Adobe DNG Converter 18.7 中仍有解码限制。

## 安装

下载 `photo_tools.wlm` 和 `SHA256SUMS.txt`，校验后替换启动卡 `FENESTRA/` 中的同名文件，再完整冷启动。r10-c1 可与 2026 年 11 月 10 日发布的其他六个 WLM 组合使用。

文件大小为 **3,041,452 字节**，SHA-256 为 `aaefcb935612b0aff2a175d19aaca4c08265095add3c8c854917fca9d6eb9b0a`。

[下载正式版](https://github.com/PIPA12344/Fenestra-Z8-C311-source/releases/tag/wlm-2026.11.20-r10-c1) · [照片工具用法](../plugins/photo_tools.md) · [源码构建](../z8-build.zh-CN.md#照片工具-r10)

## 验证

编译后的 AArch64 NEF 生成和分段写入代码通过 Unicorn 离线检查，覆盖 C120 RAW、DX C60 RAW、FX C30 RAW+JPEG、DX HE★、输入已有字段和跨 4 KiB 对齐边界。输出字段内容、RAW 和预览字节均经过核对。

正式下载文件与工作台安装并读回校验的文件逐字节一致。相机实测结果来自用户报告。
