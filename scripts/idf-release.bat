@echo off
REM Production build: secure + release overlays, in its own folder, from a fresh
REM sdkconfig. idf.py reads SDKCONFIG_DEFAULTS only when no sdkconfig exists, so
REM the README's one-liner run in a dev checkout silently builds the dev config.
REM Fails unless every release setting actually landed in the result.
setlocal
REM The signing key lives off the checkout: CRYPTNOX_KEY_DIR names its folder.
if not defined CRYPTNOX_KEY_DIR (echo Set CRYPTNOX_KEY_DIR to the folder holding secure_boot_signing_key.pem & exit /b 1)
if not exist "%CRYPTNOX_KEY_DIR%\secure_boot_signing_key.pem" (echo No secure_boot_signing_key.pem in %CRYPTNOX_KEY_DIR% & exit /b 1)
REM Kconfig strings want forward slashes.
set "KEY=%CRYPTNOX_KEY_DIR:\=/%/secure_boot_signing_key.pem"
set "MSYSTEM="
call C:\esp\v5.5.4\esp-idf\export.bat || exit /b 1
cd /d %~dp0..
if exist build-release rmdir /s /q build-release
mkdir build-release
REM Last overlay wins, so this replaces the in-repo path from the encryption overlay.
> build-release\keys.defaults echo CONFIG_SECURE_BOOT_SIGNING_KEY="%KEY%"
idf.py -B build-release -D CW_RELEASE=1 -D SDKCONFIG=build-release/sdkconfig -D "SDKCONFIG_DEFAULTS=sdkconfig.defaults;sdkconfig.defaults.flash_encryption;sdkconfig.defaults.release;build-release/keys.defaults" build || exit /b 1
findstr /c:"CW_RELEASE:UNINITIALIZED=1" build-release\CMakeCache.txt >nul || (echo RELEASE CHECK FAILED: CW_RELEASE=1 not set & exit /b 1)
findstr /c:"%KEY%" build-release\sdkconfig >nul || (echo RELEASE CHECK FAILED: signing key is not %KEY% & exit /b 1)
for %%K in ("CONFIG_SECURE_FLASH_ENC_ENABLED=y" "CONFIG_SECURE_FLASH_ENCRYPTION_MODE_RELEASE=y" "CONFIG_SECURE_BOOT_V2_ENABLED=y" "CONFIG_NVS_ENCRYPTION=y" "CONFIG_LOG_DEFAULT_LEVEL_NONE=y") do (
    findstr /x /c:%%K build-release\sdkconfig >nul || (echo RELEASE CHECK FAILED: %%~K not set & exit /b 1)
)
echo Release build OK: build-release\  (all release settings verified)
