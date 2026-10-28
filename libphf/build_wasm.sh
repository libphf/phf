#!/bin/sh
# PHF WebAssembly build (Emscripten SIMD128) -> writes apps/web/phf.js + apps/web/phf.wasm
# Requires emcc in PATH (source emsdk_env.sh first).
set -e
cd "$(dirname "$0")"
emcc -O3 -msimd128 -I include src/phf.c src/huff.c \
  -sMODULARIZE=1 \
  -sEXPORT_NAME=PHFModule \
  -sALLOW_MEMORY_GROWTH=1 \
  -sINITIAL_MEMORY=33554432 \
  -sEXPORTED_RUNTIME_METHODS=HEAPU8,HEAPU32 \
  -sEXPORTED_FUNCTIONS=_phf_encode_memory,_phf_decode_memory,_phf_probe_memory,_phf_free,_malloc,_free \
  -o ../apps/web/phf.js
echo "Built apps/web/phf.js + phf.wasm successfully."
