# Fenestra Z8 C3.11 公开源码版

Fenestra 的 Nikon Z 8 C3.11 移植。这里提供核心、WLM SDK、功能插件、Windows 调试工作台和本地构建工具的源码。

> 这是实验性代码。WonderLantern/Fenestra 已发现潜在变砖风险并要求勿刷入其当前版本。本仓库供源码研究和离线构建，尚无整机资格验证。见 [WonderLantern/Fenestra](https://github.com/WonderLantern/Fenestra)。

## 开始使用

1. **准备底座。** 当前功能 WLM 都配套 Nikon Z 8 C3.11 的 `cold-retry3`。Linux、macOS 和 Windows 可用预编译差分补丁从官方 C3.11 在本地生成底座。也可以按固定源码标签重新编译。[准备底座](docs/z8-usage.zh-CN.md#准备底座)
2. **下载整套插件。** 在 [2026-11-28 插件整合包](https://github.com/PIPA12344/Fenestra-Z8-C311-source/releases/tag/wlm-2026.11.28-current) 下载一个 ZIP，把所需的 WLM 更新到启动卡的 `FENESTRA/`，正常关机完成后重新开机（无需拔电池）。[8 项功能和整包替换步骤](docs/releases/wlm-current-20261128.md) · [复建插件](docs/wlm-current-build.zh-CN.md)
3. **机内使用。** 在设定菜单进入 Fenestra 激活，插件界面在并列的 **Z8Mouldes**。Cube LUT 入口在照片拍摄菜单。

## 历史版本 · LUT 预览 r2

11 月 23 日发布的 `lut_preview.wlm` 是整合包之前的 LUT 插件。更新到 `cube_lut.wlm` 时，请按整包说明移走旧版。[历史 Release](https://github.com/PIPA12344/Fenestra-Z8-C311-source/releases/tag/wlm-2026.11.23-lut-preview-r2)

## 调试与开发

- Windows 调试工作台由 `Start-Z8-Debug.cmd` 启动，可查看状态和构建身份，执行 RTOS 命令并读取寄存器和内存。[工作台用法](docs/z8-usage.zh-CN.md#使用-windows-调试工作台)
- 核心和插件的开发构建见[本地构建](docs/z8-build.zh-CN.md)。SDK 接口和模块格式见 [Z8 SDK](docs/z8-sdk.md) 和 [模块开发](docs/module-development.md)。
- `python -B verify_z8_source.py` 校验公开源码清单。离线测试可运行 `python -B -m unittest discover -s tests`（先创建本地 `build/` 目录）。依赖本地构建或相机原厂输入的测试会在对应输入缺席时跳过。

## 来源与许可

本移植基于 [WonderLantern/Fenestra 的 b199dcd 提交](https://github.com/WonderLantern/Fenestra/tree/b199dcdfbb0ef536c6269ae5a9d97c376d488b8b)，增加 Z8 C3.11 适配、原生插件构建和调试工作台。公开版从移植源码筛选整理。

部分 WLM 基于其他项目的功能实现移植开发。画幅、ProRes、H.265、Log 色彩和照片工具使用 NikonFirmwarePatch 的对应实现；卡内 WAV 由 z8-card-wav 适配为 WLM。[功能来源与移植](docs/z8-usage.zh-CN.md#功能来源与移植)

项目主许可是 [CC BY-NC-SA 4.0](LICENSE)，附 [商业摄影例外](EXCEPTIONS.md)。独立依赖及另有许可标注的模块按各自条款授权。署名和许可见 [NOTICE](NOTICE) 与依赖目录。
