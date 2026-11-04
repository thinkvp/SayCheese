#!/usr/bin/env bash
# Static FFmpeg for the SayCheese addon (macOS arm64, clang). The same video-only,
# LGPL configuration as build-ffmpeg-win.sh: the demuxers and decoders for the
# formats interviews are shot in, swscale, and JPEG reading and writing.
#
# Apple Silicon only (MediaPipe ships no Intel libmediapipe), so no lipo step.
# Autodetection is off so nothing links Apple's media frameworks (no hwaccel is
# used); pthreads is switched back on because decoding needs its threads.
#
# Usage: bash native/build-ffmpeg-mac.sh   ->  native/build/ffmpeg-out
# FFMPEG_SRC_DIR and FFMPEG_OUT override the source and install folders (CI).
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
VER="8.1.2"
SRC="${FFMPEG_SRC_DIR:-$HERE/build/ffmpeg-$VER}"
OUT="${FFMPEG_OUT:-$HERE/build/ffmpeg-out}"
if [ ! -d "$SRC" ]; then
  mkdir -p "$(dirname "$SRC")"
  curl -fsSL -o "$SRC.tar.xz" "https://ffmpeg.org/releases/ffmpeg-$VER.tar.xz"
  tar xf "$SRC.tar.xz" -C "$(dirname "$SRC")"
  rm "$SRC.tar.xz"
fi
cd "$SRC"

MIN_MACOS=12.0
make distclean >/dev/null 2>&1 || true
./configure \
  --prefix="$OUT" \
  --arch=arm64 \
  --cc=clang \
  --extra-cflags="-arch arm64 -mmacosx-version-min=$MIN_MACOS" \
  --extra-ldflags="-arch arm64 -mmacosx-version-min=$MIN_MACOS" \
  --disable-autodetect \
  --enable-pthreads \
  --disable-shared \
  --enable-static \
  --disable-debug \
  --disable-programs \
  --disable-doc \
  --disable-avdevice \
  --disable-avfilter \
  --disable-swresample \
  --disable-network \
  --disable-everything \
  --enable-protocol=file \
  --enable-demuxer=mov,mxf,matroska,mpegts,avi,image2 \
  --enable-decoder=h264,hevc,prores,mpeg2video,mpeg4,dnxhd,mjpeg \
  --enable-parser=h264,hevc,mpegvideo,mpeg4video,dnxhd,mjpeg \
  --enable-encoder=mjpeg > "$SRC/configure.out" || { cat "$SRC/configure.out"; tail -50 ffbuild/config.log; exit 1; }
grep -E -A1 "^(threading support|External libraries|install prefix)" "$SRC/configure.out" || true

make -j"$(sysctl -n hw.logicalcpu)"
make install
ls -l "$OUT/lib"
grep -h "Libs.private" "$OUT"/lib/pkgconfig/*.pc | sort -u
echo FFMPEG_DONE
