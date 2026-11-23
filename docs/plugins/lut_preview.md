# LUT 预览

适用于 Nikon Z 8 C3.11 的 `cold-retry3` 底座。在「照片拍摄菜单 → 选择 LUT」载入卡上的 3D LUT，即可在支持的拍摄档位预览色彩并保存到 JPEG。[下载 lut_preview.wlm](https://github.com/PIPA12344/Fenestra-Z8-C311-source/releases/download/wlm-2026.11.23-lut-preview-r2/lut_preview.wlm) · [r2 更新日志](../releases/lut-preview-r2-20261123.md)

## 哪些档位使用 LUT

| 拍摄档位 | 后屏与电子取景器 | 机内 JPEG 及内嵌预览 |
| --- | --- | --- |
| 单张 S、低速连拍 CL、高速连拍 CH，且关闭自拍 | 所选 LUT | 所选 LUT（支持的尺寸见下文） |
| 高速画面捕捉 C15、C30、C60、C120 | 原厂画面 | 原厂色彩 |
| 自拍 | 原厂画面 | 原厂色彩 |

从高速画面捕捉切回 S、CL 或 CH 后，新的取景画面和拍摄的 JPEG 会自动恢复之前选中的 LUT。

## 存储卡怎么放

```text
FENESTRA/
├─ lut_preview.wlm
├─ Portrait.cube
└─ LUTS/
   └─ Film/
      └─ Warm.cube
```

`Portrait.cube` 和 `Warm.cube` 是自备 LUT 的文件名示例。前者在机内的「根目录 LUT」中，后者在「Film」分类中。菜单读取一层分类。分类名和去掉扩展名的文件名各可用 1 至 26 个英文字母、数字、连字符或下划线。`.cube` 扩展名可用大小写。支持每轴 2 至 33 个采样点的 3D CUBE 文件，每个文件最大 2 MiB。

## 机内操作

1. 把 WLM 和 LUT 放到启动卡后完整冷启动。更新旧版时替换同名文件，旧的独立 `jpeg_lut.wlm` 需移出 `FENESTRA/`。[底座、安装和冷启动步骤](../z8-usage.zh-CN.md#安装插件)
2. 打开 **照片拍摄菜单 → 选择 LUT**，用方向键右键或 OK 进入「根目录 LUT」或分类，选中要使用的文件。一次最多显示 20 个条目，可用「上一页」和「下一页」翻页。
3. 看到「已载入」后回到取景画面拍摄。左键或触摸返回可从文件列表回到原分类。要恢复原色，在分类列表选择「无LUT」。插件将当前选择保存在这张卡上。

JPEG 主图处理路径覆盖 FX L（8256 × 5504）和 DX S（2688 × 1792），并同步处理内嵌预览。LUT 单独安装即可使用，已有照片工具 r10-c1 的用户可保留该文件。
