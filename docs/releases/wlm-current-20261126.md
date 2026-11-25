# Z8 C3.11 当前插件整合包

适用于已装 **Fenestra `cold-retry3`** 底座的 Nikon Z 8 C3.11。工作台显示的核心构建身份应为 `f06231d84970cda74ff6afcdb2aad7d03f6fe0bc4a3ec76c15a9cfd40654579a`。ZIP 只需下载一次，解压后有 8 个功能 WLM、安装说明和校验清单。

| 插件 | 功能 |
| --- | --- |
| `framing.wlm` | N-RAW 12 位 3:2 Open Gate、N-RAW 画质和反挤压。移植 v0.5.1 的 4.1K 取景与对焦及 EVF 回放修复。 |
| `prores.wlm` | ProRes 422 HQ、422、LT、Proxy 四档。 |
| `h265.wlm` | H.265 五档目标码率，按视频菜单库保存。 |
| `log_colour.wlm` | N-Log、S-Log3、Log3G10 曲线、色彩与监看。 |
| `photo_tools.wlm` | 高速 RAW、像素位移、照片裁切与 10 位 SDR HEIF。保留 r10-c1 的 NEF 兼容改动。 |
| `card_wav.wlm` | 从启动卡根目录的 `CUSTOM_SOUND/` 选择自定义 WAV 快门声。 |
| `portrait_video.wlm` | 按录像开始时的握持方向写入视频旋转信息。 |
| `cube_lut.wlm` | 在照片拍摄菜单选择 Cube LUT。支持 S、CL、CH 档的后屏、EVF 和 JPEG。C15、C30、C60、C120 及 HEIF/HIF 使用原厂色彩流程。 |

## 整包替换

1. 正常关机，等写卡灯熄灭，再取出启动卡。先把卡根目录 `FENESTRA/` 备份到电脑。
2. 将 ZIP 内 `FENESTRA/` 中的 8 个 `.wlm` 按原名复制到启动卡的 `FENESTRA/`。如果有意禁用其中某个插件，更新后保留它的 `.wlm.disabled` 后缀，同一插件只留一个副本。`framing`、`prores`、`h265`、`log_colour`、`photo_tools` 共用同一批组件，已安装的成员须在同一次冷启动前一并替换。
3. 将旧的 `lut_preview.wlm`、独立调试用的 `jpeg_lut.wlm` 及其 `.disabled` 版本移出 `FENESTRA/`，使用本包的 `cube_lut.wlm`。原有 `.cube`、`LUTS/`、`CUSTOM_SOUND/`、其他配置与照片留在原位。若要停用或卸载 `photo_tools.wlm`，先在原厂照片菜单把图像区域切回 FX。
4. 将卡装回相机，断开 USB、取下电池，装回电池后开机。已装匹配底座的机身只需更新卡上的 WLM。

## 验证

8 个 WLM 与当前实机卡上的对应文件逐块一致。整组通过生产 C 组合器及离线冷启动扫描、组合和封存检查，组合结果与 Python 参考实现逐字节一致。整组有 901 条原生记录。这一轮验证覆盖卡文件和加载流程，各项拍摄或录制功能的实机验证范围不同。

五个核心功能包基于 [NikonFirmwarePatch v0.5.1](https://github.com/XinmiaoZhao/NikonFirmwarePatch/releases/tag/v0.5.1) 移植。看 [复建步骤](../wlm-current-build.zh-CN.md) 和 [许可与来源](../../README.md#来源与许可)。
