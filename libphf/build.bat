@echo off
rem PHF (Pereira-Huffman Format) native library build (Windows MSVC + OpenMP)
rem Prefer the cross-platform CMake build (see README); this script is a shortcut.
setlocal
cd /d %~dp0

where cl.exe >nul 2>nul
if not errorlevel 1 goto compile

set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if exist "%VSWHERE%" goto findvs
echo Error: cl.exe not in PATH and vswhere.exe not found. Run from a "Developer Command Prompt".
exit /b 1

:findvs
for /f "usebackq tokens=*" %%i in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSDIR=%%i"
if not defined VSDIR (
  echo Error: no Visual Studio installation with the C++ toolset was found.
  exit /b 1
)
call "%VSDIR%\VC\Auxiliary\Build\vcvars64.bat" >nul

:compile
echo Compiling phf.dll...
cl.exe /nologo /O2 /openmp /W3 /MD /I include /DPHF_BUILD_DLL /LD src/phf.c src/huff.c /Fe:phf.dll || exit /b 1
echo Built phf.dll successfully.
