# Autonomous build + flash + monitor for the usdc_signing example.
# Sources the ESP-IDF v5.5 env via activate.py directly to avoid PowerShell
# wrapping stderr lines as errors.
#
# Usage:
#   .\auto_flash.ps1                            # build only
#   .\auto_flash.ps1 flash                      # build + flash
#   .\auto_flash.ps1 monitor                    # build + flash + monitor
#   .\auto_flash.ps1 flash COM3                 # explicit port
#   .\auto_flash.ps1 set-target esp32s3         # change target chip
#   .\auto_flash.ps1 set-target esp32           # change target chip

param(
    [string]$Action = "build",
    [string]$Port   = ""
)

# Pin the right Python venv on PATH so activate.py uses Python 3.11 (the venv
# ESP-IDF v5.5 actually has) instead of the system Python.
$idfPython = "C:\Espressif\python_env\idf5.5_py3.11_env\Scripts"
$idfGit    = "C:\Espressif\tools\idf-git\2.44.0\cmd"
$env:PATH  = "$idfPython;$idfGit;$env:PATH"
$env:IDF_PATH = "C:\Espressif\frameworks\esp-idf-v5.5"

# Call activate.py and capture only stdout (the path to the env-setup script).
# Redirect stderr to $null so the "Activating..." progress message doesn't
# trip PowerShell into raising a NativeCommandError.
$activateScript = (& python "$env:IDF_PATH\tools\activate.py" --export 2>$null | Select-Object -Last 1).Trim()
if (-not (Test-Path $activateScript)) {
    Write-Error "Could not locate activate script: '$activateScript'"
    exit 1
}
. $activateScript

switch ($Action) {
    "build"   {
        if ($Port -ne "") { idf.py -p $Port build } else { idf.py build }
    }
    "flash"   {
        if ($Port -ne "") { idf.py -p $Port build flash } else { idf.py build flash }
    }
    "monitor" {
        if ($Port -ne "") { idf.py -p $Port build flash monitor } else { idf.py build flash monitor }
    }
    "set-target" {
        # The chip name comes through $Port (we reuse the second positional).
        if ($Port -eq "") { Write-Error "set-target requires a chip name (e.g. esp32s3)"; exit 1 }
        idf.py set-target $Port
    }
    "fullclean" {
        idf.py fullclean
    }
    default   {
        Write-Host "Unknown action: $Action. Use: build | flash | monitor | set-target | fullclean"
        exit 1
    }
}
