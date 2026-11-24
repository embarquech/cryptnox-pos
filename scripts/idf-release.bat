@echo off
REM Production build: secure + release overlays, in its own folder, from a fresh
REM sdkconfig. idf.py reads SDKCONFIG_DEFAULTS only when no sdkconfig exists, so
REM the README's one-liner run in a dev checkout silently builds the dev config.
REM Fails unless every release setting actually landed in the result.
setlocal
set "MSYSTEM="
call C:\esp\v5.5.4\esp-idf\export.bat || exit /b 1
cd /d %~dp0..
if exist build-release rmdir /s /q build-release
idf.py -B build-release -D SDKCONFIG=build-release/sdkconfig -D "SDKCONFIG_DEFAULTS=sdkconfig.defaults;sdkconfig.defaults.flash_encryption;sdkconfig.defaults.release" build || exit /b 1
for %%K in ("CONFIG_SECURE_FLASH_ENC_ENABLED=y" "CONFIG_SECURE_FLASH_ENCRYPTION_MODE_RELEASE=y" "CONFIG_SECURE_BOOT_V2_ENABLED=y" "CONFIG_NVS_ENCRYPTION=y" "CONFIG_LOG_DEFAULT_LEVEL_NONE=y") do (
    findstr /x /c:%%K build-release\sdkconfig >nul || (echo RELEASE CHECK FAILED: %%~K not set & exit /b 1)
)
echo Release build OK: build-release\  (all release settings verified)
