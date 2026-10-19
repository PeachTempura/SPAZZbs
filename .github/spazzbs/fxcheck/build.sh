#!/bin/sh
# Builds the effect-parser harness from this repo's libobs sources.
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/../../.." && pwd)
L="$ROOT/libobs"
gcc -std=gnu11 -w -D_GNU_SOURCE -I "$HERE/include" -I "$L" -I "$L/graphics" \
  "$HERE/harness.c" "$HERE/stubs.c" \
  "$L/util/bmem.c" "$L/util/dstr.c" "$L/util/lexer.c" "$L/util/cf-lexer.c" "$L/util/cf-parser.c" \
  "$L/util/base.c" "$L/util/utf8.c" "$L/util/platform.c" "$L/util/platform-nix.c" \
  "$L/util/threading-posix.c" "$L/graphics/shader-parser.c" \
  -o "${1:-fxparse}" -lm -lpthread -ldl
