@echo off
REM Run a command inside the Visual Studio 2026 x64 developer environment.
REM   tools\vsenv.cmd cmake --preset clang-coverage
REM   tools\vsenv.cmd cmake --build --preset coverage
REM clang-cl, lld-link, llvm-cov and ninja all come from the VS install; nothing else is needed.
setlocal
set "NoDefaultCurrentDirectoryInExePath="
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
set "VSDIR="
for /f "usebackq delims=" %%i in (`"%VSWHERE%" -latest -property installationPath`) do set "VSDIR=%%i"
if "%VSDIR%"=="" (
  echo [vsenv] Visual Studio not found
  exit /b 1
)
call "%VSDIR%\VC\Auxiliary\Build\vcvars64.bat" >nul
if errorlevel 1 (
  echo [vsenv] vcvars64 failed
  exit /b 1
)
%*
