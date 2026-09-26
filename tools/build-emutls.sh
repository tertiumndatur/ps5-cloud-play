#!/usr/bin/env bash
set -euo pipefail

root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
sdk="$root/.deps/native/ps5-payload-sdk"
resource=$(clang-18 -print-resource-dir)
source="$resource/lib/linux/libclang_rt.builtins-x86_64.a"
[[ -f $source && -x $sdk/bin/prospero-ar ]] || {
    echo "LLVM 18 compiler runtime or PS5 SDK was not found" >&2
    exit 2
}

mkdir -p "$root/build/obj"
# PacBrew's json-c uses Clang's emulated thread-local storage ABI.
ar p "$source" emutls.c.o > "$root/build/obj/emutls.c.o"
"$sdk/bin/prospero-ar" rcs "$root/build/obj/libemutls.a" "$root/build/obj/emutls.c.o"
