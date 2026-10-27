@echo off
rem PHF (Pereira-Huffman Format) native library build (Windows MSVC + OpenMP)
call "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat" >nul
cd /d %~dp0

echo Compiling phf.dll...
cl.exe /nologo /O2 /openmp /W3 /MD /I include /DPHF_BUILD_DLL /LD src/phf.c src/huff.c /Fe:phf.dll || exit /b 1
echo Built phf.dll successfully.
