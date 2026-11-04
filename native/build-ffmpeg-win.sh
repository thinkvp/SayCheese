#!/usr/bin/env bash
# Static FFmpeg for the SayCheese addon (Windows x64, MSVC). Run through
# build-ffmpeg-win.bat, which sets up the MSVC environment first.
#
# Video only: the demuxers and decoders for the formats interviews are shot in
# (XAVC/AVC/HEVC in MP4/MOV/MXF, ProRes, DNxHD, XDCAM MPEG-2) plus swscale,
# and JPEG reading and writing for thumbnails (image2 demuxer, MJPEG encoder).
# Everything here is LGPL; no --enable-gpl.
set -e
HERE="$(cd "$(dirname "$0")" && pwd)"
SRC="$HERE/build/ffmpeg-8.1.2"
OUT="$HERE/build/ffmpeg-out"
if [ ! -d "$SRC" ]; then
  mkdir -p "$HERE/build"
  curl -fsSL -o "$HERE/build/ffmpeg.tar.xz" https://ffmpeg.org/releases/ffmpeg-8.1.2.tar.xz
  tar xf "$HERE/build/ffmpeg.tar.xz" -C "$HERE/build"
fi
cd "$SRC"

export PATH="$(dirname "$(which cl.exe)"):$PATH"

make distclean >/dev/null 2>&1 || true
./configure \
  --toolchain=msvc \
  --target-os=win64 \
  --arch=x86_64 \
  --prefix="$OUT" \
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
  --enable-encoder=mjpeg

make -j"$(nproc)"
make install
ls -l "$OUT/lib"
grep -h "Libs.private" "$OUT"/lib/pkgconfig/*.pc | sort -u
echo FFMPEG_DONE
