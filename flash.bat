@echo off
chcp 65001 >nul
cd /d "%~dp0"
echo.
echo === Flashing servo_lab.elf ===
"E:\OpenOCD\bin\openocd.exe" -f "E:\Clion\CFJ_STM32\stm32f1.cfg" -c "program build/Debug/servo_lab.elf verify reset exit" > flash_log.txt 2>&1
set RC=%ERRORLEVEL%
type flash_log.txt
echo.
if "%RC%"=="0" (echo [OK] FLASH SUCCESS) else (echo [FAIL] exit code %RC%)
if /i not "%~1"=="auto" pause