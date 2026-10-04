#!/usr/bin/env bash
# Builds Ixo for WebAssembly with Emscripten (emcc on PATH: source emsdk_env.sh).
#   playground/build.sh          the browser playground -> playground/dist/
#   playground/build.sh node     a command-line ixo that runs in Node.js -> build/wasm/
#                                (used to run the test suite on WebAssembly)
set -e
cd "$(dirname "$0")/.."

TARGET=${1:-web}
command -v emcc >/dev/null || { echo "emcc not found (install emsdk and source emsdk_env.sh)"; exit 1; }

# par workers are threads: 4 made at startup (main runs in one of them)
COMMON=(-std=c++20 -O2 -fwasm-exceptions -pthread)
SETTINGS=(-sPTHREAD_POOL_SIZE=4 -sPROXY_TO_PTHREAD=1 -sEXIT_RUNTIME=1
          -sALLOW_MEMORY_GROWTH=1 -sINITIAL_MEMORY=64MB
          -sSTACK_SIZE=8MB -sDEFAULT_PTHREAD_STACK_SIZE=8MB)

case "$TARGET" in
  web)
    OUT=playground/dist
    LINK=(-sMODULARIZE=1 -sEXPORT_NAME=createIxo -sENVIRONMENT=web,worker -sINVOKE_RUN=0
          -sEXPORTED_RUNTIME_METHODS=FS,callMain)
    ;;
  node)
    OUT=build/wasm
    LINK=(-sENVIRONMENT=node -sNODERAWFS=1)
    ;;
  *) echo "usage: playground/build.sh [web|node]"; exit 2 ;;
esac

OBJ=build/wasm-$TARGET-obj
mkdir -p "$OUT" "$OBJ"
newest_header=$(ls -t src/*.hpp | head -1)
todo=()
for src in src/*.cpp; do
  obj="$OBJ/$(basename "${src%.cpp}").o"
  if [ ! -f "$obj" ] || [ "$src" -nt "$obj" ] || [ "$newest_header" -nt "$obj" ]; then todo+=("$src"); fi
done
if [ ${#todo[@]} -gt 0 ]; then
  echo "compiling ${#todo[@]} file(s) for WebAssembly ($TARGET)..."
  export OBJ
  export FLAGS="${COMMON[*]}"
  printf '%s\n' "${todo[@]}" | xargs -P "${JOBS:-8}" -I{} sh -c \
    'obj="$OBJ/$(basename "{}" .cpp).o"; em++ $FLAGS -c "{}" -o "$obj" || { rm -f "$obj"; exit 255; }'
fi
echo "linking $OUT/ixo.js"
em++ "${COMMON[@]}" "${SETTINGS[@]}" "${LINK[@]}" -o "$OUT/ixo.js" "$OBJ"/*.o
if [ "$TARGET" = web ]; then
  cp playground/index.html playground/runner.js playground/coi-serviceworker.js "$OUT/"
  # the examples menu: programs from examples/ that need no other files
  node -e '
    const fs = require("fs");
    const names = ["hello", "fizzbuzz", "shapes", "bank", "money", "pipeline", "parallel", "game_of_life", "vectors3d"];
    const o = {};
    for (const n of names) if (fs.existsSync(`examples/${n}.ixo`)) o[n] = fs.readFileSync(`examples/${n}.ixo`, "utf8");
    fs.writeFileSync(process.argv[1], "window.IXO_EXAMPLES = " + JSON.stringify(o, null, 1) + ";\n");
  ' "$OUT/examples.js"
  echo "built $OUT/ (serve it over http, e.g. python3 -m http.server -d $OUT)"
else
  printf '#!/bin/sh\nexec node "%s" "$@"\n' "$(pwd)/$OUT/ixo.js" > "$OUT/ixo"
  chmod +x "$OUT/ixo"
  echo "built $OUT/ixo (runs $OUT/ixo.js with node)"
fi
