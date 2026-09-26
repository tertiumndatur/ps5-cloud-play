#!/usr/bin/env bash
set -euo pipefail

root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
sdk="$root/.deps/native/ps5-payload-sdk"
ports="$root/.deps/pacbrew/v0.40.2/sysroot"
source="$root/third_party/json-c-0.19"
[[ -f $source/CMakeLists.txt && -x $sdk/bin/prospero-ar && -d $ports ]] || {
    echo "PS5 SDK, PacBrew sysroot, or json-c 0.19 source is missing" >&2
    exit 2
}

export PS5_PAYLOAD_SDK="$sdk" PACBREW_SYSROOT="$ports"
cmake -S "$source" -B "$root/build/json-c-ps5" \
    -DCMAKE_TOOLCHAIN_FILE="$root/tools/chiaki-ps5-toolchain.cmake" \
    -DBUILD_SHARED_LIBS=OFF -DBUILD_STATIC_LIBS=ON \
    -DBUILD_APPS=OFF -DBUILD_TESTING=OFF -DENABLE_THREADING=OFF \
    -DDISABLE_THREAD_LOCAL_STORAGE=ON -DHAVE_ARC4RANDOM=0 -DHAVE_USELOCALE=0 \
    -DHAVE_SETLOCALE=0 -DDISABLE_WERROR=ON -DCMAKE_BUILD_TYPE=Release
cmake --build "$root/build/json-c-ps5" --target json-c -j "${BUILD_JOBS:-4}"

archive="$root/build/json-c-ps5/libjson-c.a"
if nm -u "$archive" | grep -Eq '(__emutls_get_address|uselocale|duplocale|newlocale|freelocale|arc4random)'; then
    echo "json-c archive still depends on unsupported locale, TLS, or arc4random" >&2
    exit 1
fi
