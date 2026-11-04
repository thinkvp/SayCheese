#!/usr/bin/env bash
# Builds saycheese.uxpaddon and saycheese-cli into native/build/out and copies
# the addon to plugin/mac/arm64. The macOS twin of build-addon-win.bat.
# Needs Xcode's command line tools, CMake, and the static FFmpeg from
# build-ffmpeg-mac.sh.
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
PLUGIN_ARCH="$HERE/../plugin/mac/arm64"
OUT="$HERE/build/out"

cmake -S "$HERE" -B "$HERE/build/cmake" -DCMAKE_BUILD_TYPE=Release
cmake --build "$HERE/build/cmake" -j"$(sysctl -n hw.logicalcpu)"
codesign --verify "$OUT/saycheese.uxpaddon"

# Premiere can crash if a dylib it has loaded is overwritten in place, so write
# the new addon alongside and rename it over the old one. The new one loads
# when Premiere next starts.
mkdir -p "$PLUGIN_ARCH"
cp "$OUT/saycheese.uxpaddon" "$PLUGIN_ARCH/saycheese.uxpaddon.new"
mv -f "$PLUGIN_ARCH/saycheese.uxpaddon.new" "$PLUGIN_ARCH/saycheese.uxpaddon"

# The CLI loads MediaPipe and the model from its own folder (npm run deps puts them in plugin/).
[ -f "$PLUGIN_ARCH/libmediapipe.dylib" ] && cp -f "$PLUGIN_ARCH/libmediapipe.dylib" "$OUT/"
[ -f "$HERE/../plugin/models/face_landmarker.task" ] && cp -f "$HERE/../plugin/models/face_landmarker.task" "$OUT/"
echo ADDON_BUILD_DONE
