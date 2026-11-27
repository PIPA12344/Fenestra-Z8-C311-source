# Z8 C3.11 当前插件整合包 · 2026-11-28

适用于已装 **Fenestra `cold-retry3`** 底座的 Nikon Z 8 C3.11。工作台显示的核心构建身份应为 `f06231d84970cda74ff6afcdb2aad7d03f6fe0bc4a3ec76c15a9cfd40654579a`。[下载 8 合 1 ZIP](https://github.com/PIPA12344/Fenestra-Z8-C311-source/releases/download/wlm-2026.11.28-current/Fenestra-Z8-C311-WLM-2026.11.28.zip)。解压后有 8 个功能 WLM、安装说明、许可和校验清单；Release 同时提供 8 个独立 `.wlm`、`MANIFEST.json` 和 `SHA256SUMS.txt`。

## 本次修复 · Issue #2

修复 [photo_tools 自定义裁切导致拔卡后开机黑屏、重启循环](https://github.com/PIPA12344/Fenestra-Z8-C311-source/issues/2) 的正常关机路径：

1. `nativepacks/wlm_controls/backend.c` 在 `FX_NATIVE_PACK_STOP` 正常关机时，暂存自定义照片图像区域 `0x07000603` 并重置为原厂 FX（0）。正常关机完成后，拔卡或不加载插件开机时使用 FX。热恢复时，待钩子重新就绪，在首个 `FX_NATIVE_PACK_GUI_POLL` 自动恢复暂存裁切（仅在当前仍为 FX 时恢复，避免覆盖用户已改的值）。此修复随 `photo_tools.wlm` 提供，现行 `cold-retry3` 无需重刷固件。
2. 插件页 `4:3 / 2.39:1 / 65:24` 行提示改为「照片菜单→影像区域，未加载回FX」。
3. `src/platform/z8_c311_full/menu.c` 和 `host/build_z8_core.py` 新增早期启动合法值钳位源码，覆盖活动设置及 A–D 拍摄菜单库。**这部分留待下一版底座固件发布；本 Release 只发布 WLM，现行底座尚未包含该兜底，突然断电或关机未完成的场景不能视作已修复。**
4. 旧版已陷入黑屏循环时，先切到**视频模式**开机，必要时重试；进入照片拍摄菜单，把**图像区域改回 FX**。仍无法恢复时，可用设定菜单的**重设所有设定**清除此值。恢复后更新本包。

`framing`、`prores`、`h265`、`log_colour` 本次无功能改动，但与 `photo_tools` 共享组件同批重建，**升级时必须整组替换已安装的五件套成员，正常关机完成后重新开机**。

本包还更新 `cube_lut`：仅 RAW 时保持原厂色彩；进入 C15/C30/C60/C120、自拍或仅 RAW 时归还预览和 JPEG LUT 缓冲，返回支持档位并稳定约 1 秒后重新申请。这约 1 秒内新拍的 JPEG 使用原厂色彩。`card_wav` 和 `portrait_video` 保留原有配套文件。

| 插件 | 功能 |
| --- | --- |
| `photo_tools.wlm` | v0.5.1 照片工具：高速 RAW、像素位移、自定义照片裁切、10 位 SDR HEIF；保留 r10-c1 NEF 兼容改动，本次增加裁切关机保护。 |
| `framing.wlm` | N-RAW 12 位 3:2 Open Gate、N-RAW 画质和反挤压；保留 v0.5.1 的 4.1K 取景、对焦和 EVF 回放修复。 |
| `prores.wlm` | ProRes 422 HQ、422、LT、Proxy 四档。 |
| `h265.wlm` | H.265 五档目标码率，按视频菜单库保存。 |
| `log_colour.wlm` | N-Log、S-Log3、Log3G10 曲线、色彩与监看。 |
| `cube_lut.wlm` | 在照片拍摄菜单选择 Cube LUT；支持 S、CL、CH 档后屏、EVF 和 JPEG。C15/C30/C60/C120、自拍、仅 RAW 和 HEIF/HIF 使用原厂色彩流程。 |
| `card_wav.wlm` | 从启动卡根目录 `CUSTOM_SOUND/` 选择自定义 WAV 快门声。 |
| `portrait_video.wlm` | 按录像开始时的握持方向写入视频旋转信息。 |

## 整包替换

1. 正常关机，等写卡灯熄灭，再取出启动卡。先将卡根目录 `FENESTRA/` 备份到电脑。仍用旧版自定义裁切时，更新前先把照片图像区域切回 FX。
2. 将 ZIP 中 `FENESTRA/` 的 8 个 `.wlm` 按原名复制到启动卡 `FENESTRA/`。五件套已安装成员须同一次全部替换。若有意禁用某插件，更新后保留其 `.wlm.disabled` 后缀；同一插件只留一个副本。
3. 将旧的 `lut_preview.wlm`、独立调试用 `jpeg_lut.wlm` 及其 `.disabled` 版本移出 `FENESTRA/`，使用本包 `cube_lut.wlm`。原有 `.cube`、`LUTS/`、`CUSTOM_SOUND/`、其他配置和照片留在原位。停用或卸载 `photo_tools` 前，仍建议主动在原厂照片菜单将图像区域切回 FX，并等待正常关机完成。
4. 装回启动卡后重新开机。已装匹配底座的机身只需更新卡上的 WLM；本次安装不要求拔电池。

本 ZIP **仅包含上述 8 个 WLM**。普通 EL0 示例插件 `hello.wlm`、`DEMO.WLM`、`z8_mascot.wlm` 以及 `UI_VERIFY`、`watermark` 等其他插件未包含，也未验证与其叠加的组合。现有 `dist/` 交付保持原样。

## 验证与校验

8 个指定文件的大小和 SHA-256 均与本次发布清单一致。用户已反馈这 8 个文件实机安装通过，并完成 8 包生产 C 组合校验：**900 条原生记录、695 页、0 重叠页**。本次发布另进行独立离线复建、生产 C/Python 组合比对和 ZIP 解压校验，详细结果随源码保存。此次发布过程未另行连接相机；这些证据不代表全部拍摄、录制功能或整机资格验证。

ZIP 内 `SHA256SUMS.txt` 校验包内文件；Release 外部同名清单校验 ZIP、独立 WLM 和独立 `MANIFEST.json`。五件套基于 [NikonFirmwarePatch v0.5.1](https://github.com/XinmiaoZhao/NikonFirmwarePatch/releases/tag/v0.5.1) 移植。见[复建步骤](https://github.com/PIPA12344/Fenestra-Z8-C311-source/blob/wlm-2026.11.28-current/docs/wlm-current-build.zh-CN.md)和[许可与来源](https://github.com/PIPA12344/Fenestra-Z8-C311-source/tree/wlm-2026.11.28-current#来源与许可)。
