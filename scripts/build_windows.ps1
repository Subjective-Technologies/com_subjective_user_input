param(
    [ValidateSet("msvc", "mingw")]
    [string]$Compiler = "msvc"
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

$RootDir = Split-Path -Parent $PSScriptRoot
Set-Location $RootDir

if ($Compiler -eq "msvc") {
    & "$PSScriptRoot\build_windows_msvc.ps1"
    exit $LASTEXITCODE
}

cmake -S . -B build/windows -G "MinGW Makefiles" -DCMAKE_BUILD_TYPE=Release
cmake --build build/windows

