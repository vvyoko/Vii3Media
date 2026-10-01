#!/usr/bin/env bash
# 编一份「只解 AV1、只用 dav1d」的静态 libavif，装到 $VII3_ROOT/install-libavif。
#
# 不用 MSYS2 的 libavif 包：它把 aom / rav1e / svt / libgav1 / libyuv 全打开，
# 静态库会引用这些符号（只链 dav1d 时链接失败）；而且 -lavif 会优先选导入库，
# 静态链接必须显式给 .a 路径。所以自编一份裁剪版：只有 dav1d，不要 encoder，不要 libyuv。
#
# 由 Build-Native.ps1 调用，也可以单独跑：
#   VII3_ROOT=<scratch> bash build-libavif.sh
set -e

: "${VII3_ROOT:?VII3_ROOT (scratch dir) 必须设置}"

export PATH=/mingw64/bin:/usr/bin:$PATH
export PKG_CONFIG_PATH=/mingw64/lib/pkgconfig:${PKG_CONFIG_PATH:-}

ROOT=$VII3_ROOT
VER=v1.4.2
SRC=$ROOT/libavif-1.4.2
BUILD=$ROOT/libavif-build
PREFIX=$ROOT/install-libavif
URL=https://github.com/AOMediaCodec/libavif/archive/refs/tags/$VER.tar.gz
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

mkdir -p "$ROOT"

# 1. 拉源码（仅首次，缓存在 scratch 目录）
if [ ! -d "$SRC" ]; then
  echo "== 下载 libavif $VER =="
  curl -sL -o "$ROOT/libavif.tar.gz" "$URL"
  tar -xf "$ROOT/libavif.tar.gz" -C "$ROOT"

  # 应用 vendored 补丁：放宽对「多个 (version,flags) 相同的 ipma 盒子」的拒绝。
  # ImageMagick/libheif 等编码器会把属性分进多个 ipma 盒子（对应不同 item），
  # 按 HEIF 规范应当合并；原版会以 AVIF_RESULT_BMFF_PARSE_FAILED 失败。
  PATCH="$SCRIPT_DIR/patches/libavif-allow-multiple-ipma.patch"
  if [ -f "$PATCH" ]; then
    ( cd "$SRC" && patch -p1 --no-backup-if-mismatch < "$PATCH" ) \
      && echo "== 已应用补丁：$PATCH ==" \
      || echo "warn: 应用补丁失败（$(basename "$PATCH")），请检查"
  fi
fi

# 2. 只开 dav1d 解码后端，显式关掉其他编解码器与加速库
#    （1.4.x 的取值为 OFF/SYSTEM/LOCAL，不显式关会经 pkg-config 捡起系统里的库）
if [ -d "$BUILD" ] && ! rm -rf "$BUILD" 2>/dev/null; then
  BUILD="$BUILD-$(date +%s)"
  echo "旧构建目录清理失败，改用 $BUILD"
fi
cmake -G Ninja -S "$SRC" -B "$BUILD" \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_INSTALL_PREFIX="$PREFIX" \
  -DBUILD_SHARED_LIBS=OFF \
  -DAVIF_CODEC_DAV1D=SYSTEM \
  -DAVIF_CODEC_AOM=OFF \
  -DAVIF_CODEC_LIBGAV1=OFF \
  -DAVIF_CODEC_RAV1E=OFF \
  -DAVIF_CODEC_SVT=OFF \
  -DAVIF_CODEC_AVM=OFF \
  -DAVIF_LIBYUV=OFF \
  -DAVIF_LIBSHARPYUV=OFF \
  -DAVIF_BUILD_APPS=OFF \
  -DAVIF_BUILD_TESTS=OFF \
  -DAVIF_BUILD_EXAMPLES=OFF \
  -DAVIF_BUILD_MAN_PAGES=OFF \
  -DAVIF_ENABLE_WERROR=OFF

cmake --build "$BUILD" --target install

echo "== libavif.a =="
ls -la "$PREFIX/lib/libavif.a"

# 3. 护栏：静态库里不能残留非 dav1d 的编解码器符号，否则链接 vii3_media.dll 时缺符号
if nm -u "$PREFIX/lib/libavif.a" 2>/dev/null | grep -qE 'aom_|rav1e_|svt_|libgav1_|yuv_'; then
  echo "!!! libavif.a 仍引用非 dav1d 编解码器符号："
  nm -u "$PREFIX/lib/libavif.a" | grep -E 'aom_|rav1e_|svt_|libgav1_|yuv_' | sort -u | head -20
  exit 1
fi

echo "OK：libavif 只用 dav1d，没有 aom/rav1e/svt/libgav1/libyuv 残留符号"
