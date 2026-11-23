# LUT 预览 r2

2026 年 11 月 23 日 · Nikon Z 8 C3.11 · Fenestra `cold-retry3`

- **关闭高速画面捕捉的 LUT 监看。** 切入 C15、C30、C60 或 C120 时，后屏和电子取景器从下一次显示起使用原厂画面。
- **高速档 JPEG 使用原厂色彩。** 这四个档位的机内 JPEG 主图和内嵌预览都交给相机原厂流程处理。
- **保留当前选择。** 关闭自拍、切回单张 S、低速连拍 CL 或高速连拍 CH 后，新取景画面和新拍的 JPEG 自动恢复所选 LUT，无需回菜单重选。
- 同步修正了 JPEG 色彩查找表的内存使用。插件在图像处理缓冲区内准备工作表，减少加载或切换 3D LUT 时对通用堆内存的占用。

### 更新与验证

正常关机并等写卡灯熄灭，用新版 `lut_preview.wlm` 替换启动卡 `FENESTRA/` 中的同名文件，再做一次完整冷启动。已安装 `cold-retry3` 的机身只需更新插件。[下载插件](https://github.com/PIPA12344/Fenestra-Z8-C311-source/releases/download/wlm-2026.11.23-lut-preview-r2/lut_preview.wlm) · [安装与档位说明](https://github.com/PIPA12344/Fenestra-Z8-C311-source/blob/wlm-2026.11.23-lut-preview-r2/docs/plugins/lut_preview.md)

公开源码重建文件为 **114,791 字节**，与本次同步的本地构建逐字节一致。四个高速档的取景、JPEG 原厂输出以及切回普通档恢复 LUT 均经离线回放验证。

SHA-256 为 `af9bce5b96d05d6a5c429cb63df4536b969bcb736043706755e74bf3b424abc1`。
