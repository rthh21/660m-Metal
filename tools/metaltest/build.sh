#!/bin/bash
# Builds rmb_metaltest (x86_64 macOS executable) without a macOS SDK: the program declares its
# own prototypes and links with LLVM ld64.lld against text stubs (stubs/*.tbd) that carry only the
# real install names and the imported symbols. cctools ld64 here has no libtapi (cannot read .tbd)
# and refuses a stub libSystem dylib, so stubs/*.c are not used.
# Usage: tools/metaltest/build.sh <out_dir>   (fails on any error; out_dir must not exist)
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
OUT="$1"
LLD="${LD64_LLD:-$(ls -d /usr/lib/llvm-*/bin | head -1)/ld64.lld}"
if [ -e "$OUT" ]; then echo "refusing: $OUT exists" >&2; exit 2; fi
mkdir -p "$OUT"
clang -target x86_64-apple-macos10.15 -O1 -nostdinc -fno-builtin -fno-stack-protector -fno-stack-check \
	-Wall -Werror -c "$HERE/rmb_metaltest.c" -o "$OUT/rmb_metaltest.o"
"$LLD" -arch x86_64 -platform_version macos 10.15 10.15 -e _main -o "$OUT/rmb_metaltest" "$OUT/rmb_metaltest.o" \
	"$HERE/stubs/libSystem.tbd" "$HERE/stubs/libobjc.tbd" "$HERE/stubs/Foundation.tbd" "$HERE/stubs/Metal.tbd"
echo "built $OUT/rmb_metaltest"
