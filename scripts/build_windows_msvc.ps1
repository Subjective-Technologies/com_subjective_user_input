Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

$RootDir = Split-Path -Parent $PSScriptRoot
Set-Location $RootDir

function Import-VsDevCmd {
    param(
        [string]$VsInstallDir
    )

    $vsDevCmd = Join-Path $VsInstallDir "Common7\Tools\VsDevCmd.bat"
    $vcVars = Join-Path $VsInstallDir "VC\Auxiliary\Build\vcvars64.bat"

    $batchFile = $null
    if (Test-Path $vsDevCmd) {
        $batchFile = $vsDevCmd
    } elseif (Test-Path $vcVars) {
        $batchFile = $vcVars
    } else {
        Write-Error "Could not find VsDevCmd.bat or vcvars64.bat under $VsInstallDir"
    }

    $envOutput = cmd /c "`"$batchFile`" -arch=x64 -host_arch=x64 >nul && set"
    foreach ($line in $envOutput) {
        if ($line -match "^(?<key>[^=]+)=(?<value>.*)$") {
            $key = $matches["key"]
            $value = $matches["value"]
            [Environment]::SetEnvironmentVariable($key, $value, "Process")
        }
    }
}

# Save user's VCPKG_ROOT before VS environment import (VS may overwrite it)
$savedVcpkgRoot = $env:VCPKG_ROOT

$vsInstallDir = $env:VS_INSTALL_DIR
if (-not $vsInstallDir) {
    $vsInstallDir = "C:\Program Files\Microsoft Visual Studio\18\Community"
}

Import-VsDevCmd -VsInstallDir $vsInstallDir
$env:VSINSTALLDIR = $vsInstallDir

# Restore user's VCPKG_ROOT if it was set
if ($savedVcpkgRoot) {
    $env:VCPKG_ROOT = $savedVcpkgRoot
}

if (-not (Get-Command cl.exe -ErrorAction SilentlyContinue)) {
    Write-Error "cl.exe not found after initializing VS environment. Check VS_INSTALL_DIR: $vsInstallDir"
}

if (-not $env:VCPKG_ROOT) {
    Write-Warning "VCPKG_ROOT not set. Set it to your vcpkg root to locate OpenSSL/libwebsockets."
    Write-Warning "Example: `$env:VCPKG_ROOT = 'C:\dev\vcpkg'"
}

$toolchain = $null
if ($env:VCPKG_ROOT -and (Test-Path "$env:VCPKG_ROOT\scripts\buildsystems\vcpkg.cmake")) {
    $toolchain = "$env:VCPKG_ROOT\scripts\buildsystems\vcpkg.cmake"
}

$cmakeArgs = @(
    "-S", ".",
    "-B", "build/windows-msvc",
    "-G", "NMake Makefiles",
    "-DCMAKE_BUILD_TYPE=Release"
)

if (Get-Command ninja.exe -ErrorAction SilentlyContinue) {
    $cmakeArgs = @(
        "-S", ".",
        "-B", "build/windows-msvc",
        "-G", "Ninja",
        "-DCMAKE_BUILD_TYPE=Release"
    )
}

if ($toolchain) {
    $cmakeArgs += "-DCMAKE_TOOLCHAIN_FILE=$toolchain"
}

cmake @cmakeArgs
cmake --build build/windows-msvc --config Release
