#!/usr/bin/env bash
# 构建 vii3_media.dll：裁剪过的 LGPL FFmpeg + WASAPI 音频输出 + libavif（win-x64）。
# 一个 dll 两套导出：
#   vii3_*        动态照片里的视频/音频（FFmpeg）
#   vii3_avif_*   AVIF 图像序列（libavif + dav1d）
#
# 合在一个 dll 里：FFmpeg 侧必须能解 AV1（见下面的配置说明），要链 libdav1d，
# 而 AVIF 侧也用 dav1d；分开就是两份 dav1d。
#
# 由 Build-Native.ps1 调用，也可以在 MSYS2 里单独跑：
#   VII3_ROOT=<scratch> VII3_SRC=<ffmpeg source> VII3_SHIM=<vii3_media.c> \
#   VII3_OUT=<输出目录> bash build-vii3-media.sh
#   （可选 VII3_AVIF_SHIM=<vii3_avif.c>；缺省按 ../avif/vii3_avif.c 推）
#
# 产物：$VII3_OUT/win-x64/vii3_media.dll
#
# 只支持 win-x64（WASAPI，dll 名写死）。
#
# 授权：本脚本刻意不使用 --enable-gpl / --enable-nonfree，产出的库为 LGPL。
set -e

: "${VII3_ROOT:?VII3_ROOT (scratch dir) 必须设置}"
: "${VII3_SRC:?VII3_SRC (ffmpeg source dir) 必须设置}"
: "${VII3_SHIM:?VII3_SHIM (vii3_media.c 路径) 必须设置}"
: "${VII3_OUT:?VII3_OUT (输出目录) 必须设置}"

export PATH=/mingw64/bin:/usr/bin:$PATH

OUT=$VII3_OUT/win-x64

# AVIF shim 与已编好的静态 libavif（build-libavif.sh 的产物）
VII3_AVIF_SHIM=${VII3_AVIF_SHIM:-$(cd "$(dirname "$VII3_SHIM")/../avif" && pwd)/vii3_avif.c}
LIBAVIF_INC=$VII3_ROOT/install-libavif/include
LIBAVIF_LIB=$VII3_ROOT/install-libavif/lib/libavif.a
[ -f "$VII3_AVIF_SHIM" ] || { echo "找不到 AVIF shim：$VII3_AVIF_SHIM"; exit 1; }
[ -f "$LIBAVIF_LIB" ] || { echo "找不到 $LIBAVIF_LIB（先跑 build-libavif.sh）"; exit 1; }

# 版本资源（VERSIONINFO）：可选，缺省按 Native/vii3_media.rc 推（VII3_RC 可覆盖）
VII3_RC=${VII3_RC:-$(cd "$(dirname "$VII3_SHIM")/.." && pwd)/vii3_media.rc}

# ----------------------------------------------------------------------------
# 配置（要改解码范围改这里就够了）
#
# 范围按动态照片规范定，只能多不能少：
#   容器：mov（覆盖 mp4 / m4a / 3gp / mj2）
#   视频：h264 / hevc / av1
#         AV1 必须用 libdav1d —— FFmpeg 自带的 av1 解码器只支持硬解，
#         没有 hwaccel 时解不出任何一帧（libavcodec/av1dec.c 里即写着 ENOSYS）
#   音频：aac
#
#   --disable-everything          只保留下面显式打开的部分（体积小的关键）
#   --disable-gpl / --disable-nonfree   保持 LGPL
#   --enable-swscale / --enable-swresample   像素格式转换与音频重采样（必需）
#   --disable-avdevice/network/avfilter  不需要；旋转在 native 侧按 display matrix 做
#   不启用 protocol：本库的数据全部来自调用方的 read/seek 回调，不碰文件系统
# ----------------------------------------------------------------------------
CONFIGURE_ARGS=(
  --prefix="$VII3_ROOT/install-win-x64"
  --target-os=mingw64 --arch=x86_64 --x86asmexe=nasm
  --enable-static --disable-shared
  --disable-programs --disable-doc --disable-debug
  --disable-everything
  --disable-gpl --disable-nonfree
  --disable-autodetect --disable-network
  --disable-avdevice --disable-avfilter
  --enable-swscale --enable-swresample
  --enable-demuxer=mov
  --enable-parser=h264,hevc,av1,aac
  --enable-decoder=h264,hevc,aac
  --enable-libdav1d --enable-decoder=libdav1d
  --extra-cflags="-O2"
)

# 构建目录名带「源码 + 配置」指纹：选项没变就复用（只改 shim 时只需重链），
# 一变就换新目录，避免上一次配置留下的 .o 被一起链进 dll。
CONFIG_KEY=$(printf '%s\n' "$VII3_SRC" "${CONFIGURE_ARGS[@]}" | md5sum | cut -c1-8)
BUILD=$VII3_ROOT/build-win-x64-$CONFIG_KEY
mkdir -p "$BUILD" "$OUT"
cd "$BUILD"

# 已配置过就不再跑 configure（它会重写 config.h，导致全量重编）
if [ ! -f config.h ]; then
  sh "$VII3_SRC/configure" "${CONFIGURE_ARGS[@]}" > configure.log 2>&1 \
    || { echo "CONFIGURE FAILED:"; tail -30 configure.log; exit 1; }
fi

make -j"$(nproc)" > make.log 2>&1 || { echo "MAKE FAILED:"; tail -30 make.log; exit 1; }

echo "=== link shims (win-x64) ==="
# 直接从构建树取 .o 而不是静态库：只链这一份配置真正生成的对象。
# ops_asmgen.o 是宿主侧代码生成器，不是目标代码。
# libavif 在前、libdav1d 在后，链接器按符号去重，dav1d 只进一份。
find libavcodec libavformat libavutil libswscale libswresample -name '*.o' ! -name 'ops_asmgen.o' > objects.txt

# 版本资源：编成 .o 一并链进 dll（没有 .rc 就跳过）
RES_OBJ=
if [ -f "$VII3_RC" ]; then
  windres -I /mingw64/x86_64-w64-mingw32/include -i "$VII3_RC" -o vii3_media_res.o
  RES_OBJ=vii3_media_res.o
fi

gcc -shared -O2 -fvisibility=hidden -o vii3_media.dll \
  "$VII3_SHIM" "$VII3_AVIF_SHIM" $RES_OBJ -I "$VII3_SRC" -I "$BUILD" -I "$LIBAVIF_INC" \
  @objects.txt "$LIBAVIF_LIB" /mingw64/lib/libdav1d.a /mingw64/lib/libwinpthread.a \
  -lole32 -lbcrypt -lm \
  -static-libgcc -static-libstdc++ -Wl,-s -Wl,--gc-sections 2>&1 | head -40
ls -la vii3_media.dll

cp vii3_media.dll "$OUT/"
echo "=== done: win-x64 ==="
