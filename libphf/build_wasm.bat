@echo off
rem PHF WebAssembly build (Emscripten SIMD128) -> produces phf.js and phf.wasm
cd /d %~dp0
call "..\..\emsdk\emsdk_env.bat" >nul
emcc -O3 -msimd128 -I include src/phf.c src/huff.c ^
  -sMODULARIZE=1 ^
  -sEXPORT_NAME=PHFModule ^
  -sALLOW_MEMORY_GROWTH=1 ^
  -sINITIAL_MEMORY=33554432 ^
  -sEXPORTED_RUNTIME_METHODS=HEAPU8,HEAPU32 ^
  -sEXPORTED_FUNCTIONS=_phf_encode_memory,_phf_decode_memory,_phf_probe_memory,_phf_free,_malloc,_free ^
  -o phf.js || exit /b 1
echo Built phf.js + phf.wasm successfully.
