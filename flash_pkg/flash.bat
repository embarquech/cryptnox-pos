@echo off
setlocal
cd /d "%~dp0"

echo === Cryptnox ESP32-S3 flasher ===
echo.

REM --- 1) Python is required ---
python --version >nul 2>&1
if errorlevel 1 (
    echo [ERROR] Python not found in PATH.
    echo Install Python from https://www.python.org/downloads/ ^(check "Add to PATH"^) then re-run.
    pause
    exit /b 1
)

REM --- 2) esptool: install via pip if missing ---
python -c "import esptool" >nul 2>&1
if errorlevel 1 (
    echo esptool not installed, installing via pip ^(one-time^)...
    python -m pip install --user --upgrade esptool
    if errorlevel 1 (
        echo [ERROR] pip install esptool failed.
        pause
        exit /b 1
    )
    echo.
)

REM --- 3) Flash (auto-detect COM port) ---
python -m esptool --chip esp32s3 -b 460800 ^
    --before default-reset --after hard-reset write-flash ^
    --flash-mode dio --flash-size 2MB --flash-freq 80m ^
    0x0     bootloader.bin ^
    0x8000  partition-table.bin ^
    0x10000 cryptnox-pos.bin

if errorlevel 1 (
    echo.
    echo [ERROR] Flash failed. Check that:
    echo   - The board is connected via USB.
    echo   - No other program ^(VS Code serial monitor, PuTTY^) is holding the port.
    echo   - The board is in download mode if needed ^(hold BOOT, press RESET^).
)

echo.
pause
