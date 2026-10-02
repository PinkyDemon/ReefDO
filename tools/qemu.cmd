@echo off
REM The real firmware in Espressif's QEMU (ESP32-S3), for a smoke test on this PC: emulated Ethernet instead of
REM Wi-Fi, the virtual tank instead of the probe, no Bluetooth. The page is then at http://localhost:8096/.
REM   tools\qemu.cmd            build firmware\build-qemu (sdkconfig.defaults + sdkconfig.qemu) and run it
REM   tools\qemu.cmd 8097       another host port
REM Console output goes to this window; Ctrl+A X quits QEMU.
setlocal
set "PORT=%~1"
if "%PORT%"=="" set "PORT=8096"
cd /d "%~dp0.."
set "ROOT=%CD%"
REM idf.py through the IDF's own Python: EIM's idf.py launcher drops the quotes around --qemu-extra-args.
set "IDFPY=C:\Espressif\tools\python\v6.1\venv\Scripts\python.exe C:\dev\esp\v6.1\esp-idf\tools\idf.py"
call tools\idfenv.cmd %IDFPY% -C "%ROOT%/firmware" -B "%ROOT%/firmware/build-qemu" -D "SDKCONFIG=%ROOT%/firmware/build-qemu/sdkconfig" -D "SDKCONFIG_DEFAULTS=sdkconfig.defaults;sdkconfig.qemu" qemu --qemu-extra-args="-nic user,model=open_eth,hostfwd=tcp:127.0.0.1:%PORT%-:80"
