@echo off
REM Formats every project source with the repo's .clang-format (VS's bundled clang-format).
REM   tools\vsenv.cmd tools\format.cmd
setlocal
set "LLVM=%VCToolsInstallDir%..\..\Llvm\x64\bin"
for /r core\include %%f in (*.hpp) do "%LLVM%\clang-format.exe" -i "%%f"
for /r core\src %%f in (*.cpp) do "%LLVM%\clang-format.exe" -i "%%f"
for %%f in (core\tests\*.cpp core\tests\*.hpp) do "%LLVM%\clang-format.exe" -i "%%f"
for /r firmware\main %%f in (*.cpp *.hpp) do "%LLVM%\clang-format.exe" -i "%%f"
