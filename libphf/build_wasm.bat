@echo off
rem PHF WebAssembly build (Emscripten SIMD128) -> writes apps/web/phf.js + apps/web/phf.wasm
rem Requires emcc in PATH (run emsdk_env.bat first), or EMSDK pointing at an emsdk checkout.
setlocal
cd /d %~dp0
where emcc >nul 2>nul
if not errorlevel 1 goto compile
if defined EMSDK goto useemsdk
call "..\..\emsdk\emsdk_env.bat" >nul
goto compile
:useemsdk
call "%EMSDK%\emsdk_env.bat" >nul

:compile
emcc -O3 -msimd128 -I include src/phf.c src/huff.c ^
  -sMODULARIZE=1 ^
  -sEXPORT_NAME=PHFModule ^
  -sALLOW_MEMORY_GROWTH=1 ^
  -sINITIAL_MEMORY=33554432 ^
  -sEXPORTED_RUNTIME_METHODS=HEAPU8,HEAPU32 ^
  -sEXPORTED_FUNCTIONS=_phf_encode_memory,_phf_decode_memory,_phf_probe_memory,_phf_free,_malloc,_free ^
  -o ..\apps\web\phf.js || exit /b 1
echo Built apps\web\phf.js + phf.wasm successfully.
