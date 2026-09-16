@echo off
REM Run a command inside the ESP-IDF v6.1 environment installed by EIM (IDF at C:\dev\esp, tools at C:\Espressif).
REM   tools\idfenv.cmd idf.py --version
REM   tools\idfenv.cmd idf.py -C firmware set-target esp32s3
REM   tools\idfenv.cmd idf.py -C firmware build flash monitor
REM The environment is read from EIM's own activation profile (its -e switch prints KEY=VALUE lines), so this
REM stays in sync with whatever EIM installs. PYTHONPATH is cleared first: this machine has a system-wide one
REM (SVP 4) that breaks other Pythons.
setlocal
set "PYTHONPATH="
set "NoDefaultCurrentDirectoryInExePath="
set "MSYSTEM="
set "PROFILE=C:\Espressif\tools\Microsoft.v6.1.PowerShell_profile.ps1"
if not exist "%PROFILE%" (
  echo [idfenv] EIM profile not found: %PROFILE%
  exit /b 1
)
set "SYSPATH=%PATH%"
for /f "usebackq tokens=1,* delims==" %%k in (`powershell -NoProfile -ExecutionPolicy Bypass -File "%PROFILE%" -e`) do (
  if /i "%%k"=="PATH" (set "PATH=%%l;%SYSPATH%") else if /i not "%%k"=="SYSTEM_PATH" set "%%k=%%l"
)
%*
