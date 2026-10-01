# Vii3Media — 原生媒体解码库（对应源码）

本仓库是 **vii3_media.dll** 的完整对应源码。该 dll 随 Vii3 应用分发，公开本仓库的目的是
让任何人都能自行编译它、修改其中的 FFmpeg / libavif / dav1d 并重新链接、替换该 dll。

## 内容

| 路径 | 说明 |
|---|---|
| `Native/media/vii3_media.c` | 视频 / 音频解码封装（FFmpeg shim，导出 `vii3_*`） |
| `Native/avif/vii3_avif.c` | AVIF 图像序列解码封装（libavif shim，导出 `vii3_avif_*`） |
| `Native/v3_hdr.h` | HDR 色调映射与高位深工具的共享实现 |
| `Native/v3_pool.h` | 逐行变换的轻量线程池 |
| `Native/vii3_media.rc` | dll 版本资源 |
| `Build/Build-Native.ps1` | 构建入口（Windows + MSYS2） |
| `Build/native/*.sh` | FFmpeg / libavif 的配置与链接脚本 |
| `Build/native/patches/` | 对第三方源码的补丁 |

## 构建

先装 MSYS2 与依赖（见 `Build/Build-Native.ps1` 顶部的 PREREQUISITES），然后：

```powershell
.\Build\Build-Native.ps1
```

- 产物：`dist\win-x64\vii3_media.dll`（用 `-OutDir` 可改输出目录）。
- FFmpeg / libavif 源码由脚本按固定版本下载并缓存在 `Build\.native-build`；
  已在别处构建过时，可用 `-WorkRoot <dir>` 复用缓存、`-SkipLibAvif` 跳过 libavif。
- 只支持 win-x64（WASAPI，dll 名写死）。

## 授权

- 本仓库自有代码（`Native/` 的 shim、`Build/` 的脚本）：**MIT**，见 `LICENSE`。
  这样任何人都可以修改并重新编译整个 dll。
- 静态链入的第三方组件：FFmpeg（LGPL 2.1 或更高，原文见 `COPYING.LGPLv2.1`）、
  dav1d（BSD-2-Clause）、libavif（BSD-2-Clause）。详见 `THIRD-PARTY.md`。

## 替换方式

应用通过 P/Invoke 动态加载 `vii3_media.dll`（位于应用的 `runtimes\win-x64\`）。
按上面的步骤重新构建后，用产物覆盖该文件即可生效；应用与本仓库之间没有其他耦合。
