@echo off
REM Compile and upload Scott Adams sketch to ESP32-8048S043C (Sunton 4.3" RGB)
REM
REM Usage:
REM   build.bat                    - compile + upload (default port)
REM   build.bat compile            - compile only
REM   build.bat upload             - upload only
REM   build.bat monitor            - open serial monitor
REM   build.bat all                - compile + upload + monitor
REM
REM Each form accepts an optional COM port as the next arg (handy
REM because Windows reassigns the port on every replug):
REM   build.bat upload  COM23
REM   build.bat all     COM7
REM   build.bat         COM5      (compile + upload on COM5)
REM   build.bat monitor COM4

setlocal enabledelayedexpansion

set "DEFAULT_PORT=COM17"
set "ACTION=%~1"
set "PORT_ARG=%~2"

REM If the user passed only a COM port and no action, treat the first
REM arg as the port instead.
if "!PORT_ARG!"=="" (
  set "FIRST=!ACTION!"
  if /i "!FIRST:~0,3!"=="COM" (
    set "PORT_ARG=!ACTION!"
    set "ACTION="
  )
)

if "!PORT_ARG!"=="" (
  set "PORT=!DEFAULT_PORT!"
) else (
  set "PORT=!PORT_ARG!"
)

set "FQBN=esp32:esp32:esp32s3:PSRAM=opi,FlashSize=16M,PartitionScheme=app3M_fat9M_16MB,CDCOnBoot=default"
set "SKETCH_DIR=%~dp0"
if "!SKETCH_DIR:~-1!"=="\" set "SKETCH_DIR=!SKETCH_DIR:~0,-1!"

echo.
echo === build.bat: action='!ACTION!' port='!PORT!' ===

if "!ACTION!"==""        goto compile_upload
if /i "!ACTION!"=="compile" goto compile
if /i "!ACTION!"=="upload"  goto upload
if /i "!ACTION!"=="monitor" goto monitor
if /i "!ACTION!"=="all"     goto all
goto compile_upload

:compile
echo.
echo === Compiling ===
arduino-cli compile --fqbn !FQBN! --libraries "!SKETCH_DIR!" "!SKETCH_DIR!"
goto end

:upload
echo.
call :killmonitor
echo === Uploading to !PORT! ===
arduino-cli upload -p !PORT! --fqbn !FQBN! "!SKETCH_DIR!"
goto end

:compile_upload
echo.
echo === Compiling ===
arduino-cli compile --fqbn !FQBN! --libraries "!SKETCH_DIR!" "!SKETCH_DIR!"
if errorlevel 1 goto end
echo.
call :killmonitor
echo === Uploading to !PORT! ===
arduino-cli upload -p !PORT! --fqbn !FQBN! "!SKETCH_DIR!"
goto end

:monitor
echo.
echo === Monitoring !PORT! (Ctrl+C to exit) ===
arduino-cli monitor -p !PORT! --config baudrate=115200
goto end

:all
echo.
echo === Compiling ===
arduino-cli compile --fqbn !FQBN! --libraries "!SKETCH_DIR!" "!SKETCH_DIR!"
if errorlevel 1 goto end
echo.
call :killmonitor
echo === Uploading to !PORT! ===
arduino-cli upload -p !PORT! --fqbn !FQBN! "!SKETCH_DIR!"
if errorlevel 1 goto end
echo.
echo === Monitoring !PORT! (Ctrl+C to exit) ===
arduino-cli monitor -p !PORT! --config baudrate=115200
goto end

REM Kill any lingering arduino-cli monitor process so the COM port is
REM free for the upload step. Without this, an open `build.bat monitor`
REM (or any other arduino-cli serial monitor) holds the port and the
REM upload fails with "Access is denied". By the time we call this
REM helper, the compile step has already finished, so any remaining
REM arduino-cli.exe is a stale monitor — safe to terminate.
:killmonitor
tasklist /FI "IMAGENAME eq arduino-cli.exe" 2>nul | find /I "arduino-cli.exe" >nul
if not errorlevel 1 (
  echo Closing serial monitor on !PORT!...
  taskkill /F /IM arduino-cli.exe >nul 2>&1
  REM Brief settle so the OS releases the COM handle before upload
  timeout /t 1 /nobreak >nul 2>&1
)
exit /b 0

:end
endlocal
