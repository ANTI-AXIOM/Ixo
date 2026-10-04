#!/usr/bin/env bash
# Builds Ixo with g++ (or $CXX). Incremental and parallel.
#   ./build.sh           release build  -> build/release/ixo(.exe)
#   ./build.sh debug     debug build    -> build/debug/ixo(.exe)
#   ./build.sh clean
set -e
cd "$(dirname "$0")"

MODE=${1:-release}
if [ "$MODE" = clean ]; then rm -rf build; echo "cleaned"; exit 0; fi

CXX=${CXX:-g++}
FLAGS="-std=c++20 -Wall -Wextra -Wno-unused-parameter"
case "$MODE" in
  release) FLAGS="$FLAGS -O2" ;;
  debug)   FLAGS="$FLAGS -O0 -g" ;;
  *) echo "usage: ./build.sh [release|debug|clean]"; exit 2 ;;
esac

OUT=${IXO_BUILD_DIR:-build/$MODE}
mkdir -p "$OUT/obj"
EXE=$OUT/ixo
LDFLAGS="-pthread -ldl"
case "$(uname -s 2>/dev/null)" in
  MINGW*|MSYS*|CYGWIN*)
    EXE=$OUT/ixo.exe
    LDFLAGS="-static -lwinhttp -lws2_32"
    # MinGW's g++ is a native Windows program: an MSYS-style TMPDIR (/c/...)
    # makes it fail silently, so let it fall back to the Windows TEMP folder.
    unset TMPDIR
    ;;
esac
LDFLAGS="$LDFLAGS ${IXO_LDFLAGS:-}"  # e.g. -static-libstdc++ for release binaries

# A changed header rebuilds everything (simple and safe).
newest_header=$(ls -t src/*.hpp | head -1)
todo=()
for src in src/*.cpp; do
  obj="$OUT/obj/$(basename "${src%.cpp}").o"
  if [ ! -f "$obj" ] || [ "$src" -nt "$obj" ] || [ "$newest_header" -nt "$obj" ]; then
    todo+=("$src")
  fi
done

if [ ${#todo[@]} -gt 0 ]; then
  echo "compiling ${#todo[@]} file(s) ($MODE)..."
  export CXX FLAGS OUT
  printf '%s\n' "${todo[@]}" | xargs -P "${JOBS:-8}" -I{} sh -c \
    'obj="$OUT/obj/$(basename "{}" .cpp).o"; $CXX $FLAGS -c "{}" -o "$obj" || { rm -f "$obj"; exit 255; }'
fi

if [ ! -f "$EXE" ] || [ -n "$(find "$OUT/obj" -name '*.o' -newer "$EXE" 2>/dev/null | head -1)" ] || [ ${#todo[@]} -gt 0 ]; then
  echo "linking $EXE"
  objs=()
  for src in src/*.cpp; do objs+=("$OUT/obj/$(basename "${src%.cpp}").o"); done
  $CXX $FLAGS -o "$EXE" "${objs[@]}" $LDFLAGS
fi
echo "built $EXE"
