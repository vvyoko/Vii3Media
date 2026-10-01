# 第三方组件

`vii3_media.dll` 静态链入以下组件。本文件说明各自的授权与出处。

## FFmpeg（LGPL 2.1 或更高）

- 版本：**9.0.1**（上游 tarball，**未做任何修改**）
- 来源：<https://ffmpeg.org/releases/ffmpeg-9.0.1.tar.xz>
- 构建配置见 `Build/native/build-vii3-media.sh`：`--disable-everything` 后只开
  mov 解复用、h264/hevc/av1(libdav1d) 解码、aac 解码、swscale、swresample；
  **未启用 `--enable-gpl` 与 `--enable-nonfree`**。
- 许可原文：`LICENSES/COPYING.LGPLv2.1`

## libavif（BSD-2-Clause）

- 版本：**1.4.2**
- 来源：<https://github.com/AOMediaCodec/libavif>（tag v1.4.2）
- 补丁：`Build/native/patches/libavif-allow-multiple-ipma.patch`
- 许可原文：`LICENSES/libavif-LICENSE.txt`

## dav1d（BSD-2-Clause）

- 来源：MSYS2 包 `mingw-w64-x86_64-dav1d`（VideoLAN 及 dav1d 作者）
- 许可原文：`LICENSES/dav1d-LICENSE.txt`

## 关于 FFmpeg 的 LGPL

本 dll 以**静态链接**方式包含 FFmpeg（LGPL），所以发布本 dll 时除了附上许可原文，
还必须让接收方能够**修改 FFmpeg 并重新链接**出可用的 dll。本仓库正是为此而公开：

- `Native/`、`Build/` —— 本仓库自有代码与全套构建脚本（MIT 授权，见 `LICENSE`）；
- FFmpeg 源码 —— 由上表版本与地址获取，未做修改；
- libavif / dav1d —— 同样由脚本按固定版本获取。

据此任何人只需装好 MSYS2 依赖，运行 `Build/Build-Native.ps1` 即可重建一个功能等价的
`vii3_media.dll`，并替换应用目录下的同名文件。
