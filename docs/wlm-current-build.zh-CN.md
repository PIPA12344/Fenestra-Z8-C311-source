# 复建 2026-11-28 插件整合包

使用这个 Release 的源码标签、Python 3.11+ 和 Windows x64 LLVM 19.1.5。准备以下本地输入。

- Nikon Z 8 C3.11 原厂 MAIN。按[提取步骤](z8-build.zh-CN.md)生成 `inputs/z8-c311-main.bin`，SHA-256 为 `2b43a31c9de838f84b12416de4348a7004e65be07f853ec56ec2b10f3a8e3853`。
- `cold-retry3` MAIN。按[底座复建说明](z8-usage.zh-CN.md#从源码重新编译)用固定标签单独生成，SHA-256 为 `0e0114267ef82caf78b7795a470192b434528ec0f4a8a6b031d1a003c4c63dfd`。
- 已发布的 [Photo Tools r10-c1](https://github.com/PIPA12344/Fenestra-Z8-C311-source/releases/download/wlm-2026.11.20-r10-c1/photo_tools.wlm)，放到 `inputs/r10-c1/photo_tools.wlm`。构建器核对 SHA-256 后读取保留的捕获窗口和菜单数据。
- [NikonFirmwarePatch v0.5.1 工具包](https://github.com/XinmiaoZhao/NikonFirmwarePatch/releases/tag/v0.5.1) 的 `NikonFirmwarePatch-v0.5.1-toolkit.zip`，放到 `inputs/`。其中的预编译功能单元和源码分别受上游许可约束。

在源码目录用 PowerShell 执行。将 `$llvm`、`$foundation` 改为本机路径，每次构建使用新输出目录。

```powershell
$ErrorActionPreference = 'Stop'
$PSNativeCommandUseErrorActionPreference = $true
$llvm = 'C:/Program Files/Microsoft Visual Studio/2022/Community/VC/Tools/Llvm/x64/bin'
$foundation = '../Fenestra-cold-retry3/build/cold-retry3/core/z8-core.main'
$stock = 'inputs/z8-c311-main.bin'
$photo = 'inputs/r10-c1/photo_tools.wlm'
$work = 'build/current-wlm'
$env:FENESTRA_V051_CLANG = Join-Path $llvm 'clang.exe'

python -B scripts/prepare_z8_feature_toolkit.py --v051 inputs/NikonFirmwarePatch-v0.5.1-toolkit.zip
python -B host/build_z8_card_wav.py --stock $stock --foundation $foundation --llvm-bin $llvm --output "$work/wav"
python -B host/build_z8_portrait_video.py --stock $stock --foundation $foundation --llvm-bin $llvm --output "$work/portrait"
python -B host/build_z8_photo_menu.py --stock $stock --foundation $foundation --photo-tools $photo --llvm-bin $llvm --output "$work/cube"

python -B nativepacks/wlm_controls/build.py --component all --suite v051 --llvm $llvm --out "$work/controls"
python -B nativepacks/wlm_controls/build.py --component locale --suite v051 --llvm $llvm --out "$work/controls/locale"
python -B host/build_wlm_native_components.py --suite v051 --stock $stock --foundation $foundation --photo $photo --controls "$work/controls" --out "$work/catalog" --coalesce-records --peer-wlms "$work/cube/cube_lut.wlm" "$work/portrait/portrait_video.wlm" "$work/wav/card_wav.wlm"
python -B host/package_wlm_native_components.py --catalog "$work/catalog" --out "$work/components"

$ids = 'framing', 'prores', 'h265', 'log_colour', 'photo_tools'
$frontends = foreach ($id in $ids) {
    $source = if ($id -eq 'photo_tools') { 'modules/photo_tools/r10/module.c' } else { "examples/$id/module.c" }
    $out = "$work/frontend-set/frontends/$id"
    python -B host/build_sdk_module.py $source --id $id --required 0x3f0000 --target z8-c311 --optimize s --llvm-bin $llvm --output-dir $out | Out-Host
    (Get-Content "$out/module.json" -Raw | ConvertFrom-Json).modules[0]
}
@{frontends = @($frontends)} | ConvertTo-Json -Depth 4 | Set-Content -Encoding utf8 "$work/frontend-set/artifact-catalog.json"
python -B host/package_z8_wlm_plugins.py --components "$work/components" --frontends "$work/frontend-set" --output "$work/plugins"

New-Item -ItemType Directory -Force "$work/final/FENESTRA" | Out-Null
Copy-Item "$work/plugins/FENESTRA/*.wlm" "$work/final/FENESTRA/"
python -B host/repack_wlm_records.py "$work/wav/card_wav.wlm" "$work/final/FENESTRA/card_wav.wlm"
python -B host/repack_wlm_records.py "$work/cube/cube_lut.wlm" "$work/final/FENESTRA/cube_lut.wlm"
Copy-Item "$work/portrait/portrait_video.wlm" "$work/final/FENESTRA/"
Get-ChildItem "$work/final/FENESTRA/*.wlm" | Get-FileHash -Algorithm SHA256
```

对照发布 ZIP 内的 `SHA256SUMS.txt` 校验 8 个输出。五件套在生成原生胶囊时使用 `--coalesce-records`，再直接打包；不要沿用上一版的 `--no-coalesce-records` 加事后合并流程，否则语义相同的记录也可能产生不同的包身份。Cube LUT 与 WAV 在打包后分别合并记录。查看[功能和安装说明](releases/wlm-current-20261128.md)。

## 离线校验

```powershell
python -B scripts/verify_wlm_release.py --wlms "$work/final/FENESTRA" --manifest docs/releases/wlm-current-20261128-manifest.json --output "$work/verify" --foundation $foundation
python -B -m pytest -p no:faulthandler -p no:cacheprovider tests/test_photo_crop_shutdown.py tests/test_z8_lut_release.py tests/test_wlm_native.py
```

裁切回归默认读取 `build/release-20261128/controls`；使用其他目录时，先设置 `$env:FX_WLM_CONTROLS = "$work/controls"`。测试执行编译后的插件指令，Data Store 与 GUI/RTOS 边界由合成夹具提供，不连接相机。

[本次离线结果](releases/wlm-current-20261128-verification.json)：8 包生产 C/Python 组合逐字节一致，900 条原生记录、695 个记录目标页（另有加载器自身的撤销页），五件套与配套插件的 IMAGE 页交集为 0；first-fit/best-fit 冷扫描、组合和封存均通过。原厂 MAIN 可映射的前像逐一核对，其余动态根仅允许构建器明确列出的 bootstrap 根。

本标签也提交了下一版底座的早期启动裁切钳位源码。本次只复建和发布 WLM；复建兼容底座仍使用固定 `cold-retry3-source` 标签，不能把本标签构建出的新核心称为现行 cold-retry3。

[上一版复建说明](wlm-build-20261126.zh-CN.md)保持为历史记录。
