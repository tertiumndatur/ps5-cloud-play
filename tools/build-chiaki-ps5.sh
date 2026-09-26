#!/usr/bin/env bash
set -euo pipefail

root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
sdk="$root/.deps/native/ps5-payload-sdk"
ports="$root/.deps/pacbrew/v0.40.2/sysroot"
[[ -x $sdk/bin/prospero-ar ]] || bash "$root/tools/setup-native-dependencies.sh" >/dev/null
[[ -d $ports/user/homebrew/lib ]] || bash "$root/tools/setup-pacbrew-dependencies.sh" --all >/dev/null

export PS5_PAYLOAD_SDK="$sdk" PACBREW_SYSROOT="$ports"
export PKG_CONFIG_DIR= PKG_CONFIG_SYSROOT_DIR="$ports"
export PKG_CONFIG_LIBDIR="$ports/user/homebrew/lib/pkgconfig"
export PKG_CONFIG_PATH="$ports/user/homebrew/libdata/pkgconfig"

cmake -S "$root/third_party/chiaki-cloud" -B "$root/build/chiaki-ps5" \
  -DCMAKE_TOOLCHAIN_FILE="$root/tools/chiaki-ps5-toolchain.cmake" \
  -DCHIAKI_PS5=ON \
  -DCHIAKI_ENABLE_TESTS=OFF -DCHIAKI_ENABLE_CLI=OFF \
  -DCHIAKI_ENABLE_GUI=OFF -DCHIAKI_ENABLE_BOREALIS=OFF \
  -DCHIAKI_ENABLE_ANDROID=OFF -DCHIAKI_ENABLE_STEAMDECK_NATIVE=OFF \
  -DCHIAKI_ENABLE_SETSU=OFF -DCHIAKI_ENABLE_SPEEX=OFF \
  -DCHIAKI_ENABLE_STEAM_SHORTCUT=OFF -DCHIAKI_ENABLE_PI_DECODER=OFF \
  -DCHIAKI_ENABLE_FFMPEG_DECODER=OFF \
  -DCHIAKI_USE_SYSTEM_CURL=ON -DCHIAKI_USE_SYSTEM_JERASURE=OFF \
  -DCHIAKI_USE_SYSTEM_NANOPB=OFF \
  -DCHIAKI_LIB_MINIUPNPC_EXTERNAL_PROJECT=ON \
  -DCHIAKI_LIB_ENABLE_OPUS=ON \
  -DCMAKE_BUILD_TYPE=Release
cmake --build "$root/build/chiaki-ps5" --target chiaki-lib -j "${BUILD_JOBS:-4}"
