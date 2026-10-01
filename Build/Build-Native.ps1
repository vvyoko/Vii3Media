<#
.SYNOPSIS
  构建 win-x64 本地库 vii3_media.dll。

.DESCRIPTION
  产物：<OutDir>\win-x64\vii3_media.dll（OutDir 缺省为 <仓库>\dist）
    裁剪版 LGPL FFmpeg（mov/mp4 + h264/hevc/av1 + aac + swresample + swscale）
    + WASAPI 音频输出 + libavif/dav1d，一个 dll 两套导出：
      vii3_*        动态照片里的视频/音频。源码：Native\media\vii3_media.c
      vii3_avif_*   AVIF 图像序列（透明 + 动画）。源码：Native\avif\vii3_avif.c
    解码范围见 native\build-vii3-media.sh 里的配置说明。

  合在一个 dll 里：FFmpeg 侧必须能解 AV1，要链 libdav1d，而 AVIF 侧也用 dav1d，
  分开就是两份 dav1d。

  构建顺序（native\ 下两个脚本）：
    build-libavif.sh      先编裁剪版静态 libavif（只用 dav1d）到 <scratch>\install-libavif
    build-vii3-media.sh   编 FFmpeg + 链两份 shim + libavif + dav1d → vii3_media.dll

  只构建 win-x64（WASAPI，dll 名写死）。

.PREREQUISITES (one time)
  Windows 主机 + MSYS2（https://www.msys2.org；安装位置用 -Msys2Root 或 $env:MSYS2_ROOT 指定）：
    pacman -Syu
    pacman -S --needed base-devel mingw-w64-x86_64-gcc mingw-w64-x86_64-nasm `
                     mingw-w64-x86_64-dav1d mingw-w64-x86_64-cmake mingw-w64-x86_64-ninja

  libavif 不需要装包（脚本自己下 v1.4.2 源码编裁剪版，源码缓存在 scratch 目录）。

.PARAMETER Msys2Root
  MSYS2 根目录。留空时依次取 $env:MSYS2_ROOT、D:\BACK\mingw64、D:\msys64、C:\msys64。

.PARAMETER FfmpegVersion
  FFmpeg 源码版本（默认 9.0.1）。

.PARAMETER WorkRoot
  构建缓存根目录：FFmpeg 源码与构建树、libavif 源码/构建树/install 都放在它下面。
  留空时依次取 $env:VII3_BUILD_ROOT、<仓库>\Build\.native-build。
  不要放在 %TEMP% 下：它会被系统清理，导致下次重新下载并全量重编 FFmpeg。

.PARAMETER OutDir
  产物输出目录（缺省 <仓库>\dist）。产物落在它的 win-x64\ 子目录下。

.PARAMETER SkipLibAvif
  复用缓存里已有的 <WorkRoot>\install-libavif（跳过裁剪版 libavif 的重新编译）。

.EXAMPLE
  .\Build-Native.ps1
.EXAMPLE
  .\Build-Native.ps1 -SkipLibAvif   # 只重编 vii3_media.dll
#>
param (
    [Parameter()]
    [string]$Msys2Root = "",

    [Parameter()]
    [string]$FfmpegVersion = "9.0.1",

    [Parameter()]
    [string]$WorkRoot = "",

    [Parameter()]
    [string]$OutDir = "",

    [Parameter()]
    [switch]$SkipLibAvif
)

$ErrorActionPreference = "Stop"

# --- 路径 ---
$scriptRoot = $PSScriptRoot
$repoRoot = [System.IO.Path]::GetFullPath((Join-Path $scriptRoot ".."))
$mediaShim = Join-Path $repoRoot "Native\media\vii3_media.c"
$avifShim = Join-Path $repoRoot "Native\avif\vii3_avif.c"
if ([string]::IsNullOrEmpty($OutDir)) {
    $mediaOutDir = Join-Path $repoRoot "dist"
} else {
    $mediaOutDir = [System.IO.Path]::GetFullPath($OutDir)
}

foreach ($shim in @($mediaShim, $avifShim)) {
    if (-not (Test-Path $shim)) { Write-Error "shim source not found: $shim" }
}

# --- 定位 MSYS2（依次尝试 MSYS2_ROOT、D:\BACK\mingw64、D:\msys64、C:\msys64）---
if ([string]::IsNullOrEmpty($Msys2Root)) {
    $candidates = @($env:MSYS2_ROOT, "D:\BACK\mingw64", "D:\msys64", "C:\msys64") |
        Where-Object { -not [string]::IsNullOrEmpty($_) }
    $Msys2Root = $candidates |
        Where-Object { Test-Path (Join-Path $_ "usr\bin\bash.exe") } |
        Select-Object -First 1
    if ([string]::IsNullOrEmpty($Msys2Root)) { $Msys2Root = "C:\msys64" }
}

$bash = Join-Path $Msys2Root "usr\bin\bash.exe"
if (-not (Test-Path $bash)) {
    Write-Error @"
MSYS2 not found at $Msys2Root.
用 -Msys2Root 指定安装位置，并先装依赖：
  pacman -S --needed base-devel mingw-w64-x86_64-gcc mingw-w64-x86_64-nasm mingw-w64-x86_64-dav1d mingw-w64-x86_64-cmake mingw-w64-x86_64-ninja
"@
}

# --- 构建缓存根（源码 + 编译树 + libavif install）---
if ([string]::IsNullOrEmpty($WorkRoot)) {
    if (-not [string]::IsNullOrEmpty($env:VII3_BUILD_ROOT)) { $WorkRoot = $env:VII3_BUILD_ROOT }
    else { $WorkRoot = Join-Path $repoRoot "Build\.native-build" }
}
$workRoot = [System.IO.Path]::GetFullPath($WorkRoot)
New-Item -ItemType Directory -Force -Path $workRoot | Out-Null
$sourceDir = Join-Path $workRoot "ffmpeg-$FfmpegVersion"
Write-Host "构建缓存目录: $workRoot" -ForegroundColor DarkGray
Write-Host "输出目录:     $mediaOutDir" -ForegroundColor DarkGray

function ConvertTo-ShellPath([string]$path) {
    # MSYS2 bash 要 /c/... 形式
    $full = [System.IO.Path]::GetFullPath($path)
    $drive = $full.Substring(0, 1).ToLowerInvariant()
    return "/$drive/" + ($full.Substring(3) -replace '\\', '/')
}

function Invoke-BuildScript([string]$script) {
    $env:MSYSTEM = "MINGW64"
    $env:CHERE_INVOKING = "1"
    $sh = ConvertTo-ShellPath $script
    & $bash -lc "bash '$sh'"
    if ($LASTEXITCODE -ne 0) { throw "Build failed: $script" }
}

# 1. 取 FFmpeg 源码（缓存命中则跳过）
$sourceReady = (Test-Path (Join-Path $sourceDir "configure"))
if (-not $sourceReady) {
    if (Test-Path $sourceDir) { Remove-Item -Recurse -Force $sourceDir }

    $tarball = Join-Path $workRoot "ffmpeg-$FfmpegVersion.tar.xz"
    if (-not (Test-Path $tarball)) {
        Write-Host "Downloading FFmpeg $FfmpegVersion source..."
        Invoke-WebRequest -Uri "https://ffmpeg.org/releases/ffmpeg-$FfmpegVersion.tar.xz" -OutFile $tarball
    }

    Write-Host "Extracting FFmpeg source..."
    # 用 MSYS2 的 tar（带 xz）：Windows 内置 bsdtar 会外调不存在的 xz
    $shTarball = ConvertTo-ShellPath $tarball
    $shWork = ConvertTo-ShellPath $workRoot
    & $bash -lc "tar -xf '$shTarball' -C '$shWork'"
    if ($LASTEXITCODE -ne 0 -or -not (Test-Path (Join-Path $sourceDir "configure"))) {
        throw "Failed to extract FFmpeg source into $workRoot"
    }
}

$env:VII3_ROOT = ConvertTo-ShellPath $workRoot
$env:VII3_SRC = ConvertTo-ShellPath $sourceDir

$libavifScript = Join-Path $scriptRoot "native\build-libavif.sh"
$mediaScript = Join-Path $scriptRoot "native\build-vii3-media.sh"

# 0. 裁剪版 libavif（只用 dav1d）
if (-not $SkipLibAvif) {
    Write-Host ""
    Write-Host "########## libavif (dav1d only) ##########" -ForegroundColor Cyan
    Invoke-BuildScript $libavifScript
}

# 1. vii3_media.dll（视频 + WASAPI 音频输出 + AVIF）
Write-Host ""
Write-Host "########## vii3_media (win-x64) ##########" -ForegroundColor Cyan
$env:VII3_SHIM = ConvertTo-ShellPath $mediaShim
$env:VII3_AVIF_SHIM = ConvertTo-ShellPath $avifShim
$env:VII3_OUT = ConvertTo-ShellPath $mediaOutDir
Invoke-BuildScript $mediaScript

Write-Host ""
Write-Host "Build complete:" -ForegroundColor Green
if (-not (Test-Path $mediaOutDir)) {
    Write-Warning "no output produced under $mediaOutDir"
} else {
    Get-ChildItem $mediaOutDir -Recurse -File | ForEach-Object {
        "{0,-52} {1,8:N2} MB" -f $_.FullName.Substring($repoRoot.Length), ($_.Length / 1MB)
    }
}
