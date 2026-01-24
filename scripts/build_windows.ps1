param(
    [ValidateSet("msvc", "mingw")]
    [string]$Compiler = "msvc"
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

$RootDir = Split-Path -Parent $PSScriptRoot
Set-Location $RootDir

if ($Compiler -eq "msvc") {
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

    $vsInstallDir = $env:VS_INSTALL_DIR
    if (-not $vsInstallDir) {
        $vsInstallDir = "C:\Program Files\Microsoft Visual Studio\18\Community"
    }

    Import-VsDevCmd -VsInstallDir $vsInstallDir
    $env:VSINSTALLDIR = $vsInstallDir

    if (-not (Get-Command cl.exe -ErrorAction SilentlyContinue)) {
        Write-Error "cl.exe not found after initializing VS environment. Check VS_INSTALL_DIR: $vsInstallDir"
    }

    $commonVcpkgPaths = @(
        "C:\dev\vcpkg",
        "C:\vcpkg",
        "$env:USERPROFILE\vcpkg",
        "$env:LOCALAPPDATA\vcpkg"
    )

    $preferredVcpkgRoot = $null
    foreach ($path in $commonVcpkgPaths) {
        if (Test-Path "$path\scripts\buildsystems\vcpkg.cmake") {
            if (Test-Path "$path\installed\x64-windows\include\openssl") {
                $preferredVcpkgRoot = $path
                break
            }
            if (-not $preferredVcpkgRoot) {
                $preferredVcpkgRoot = $path
            }
        }
    }

    if ($preferredVcpkgRoot) {
        $env:VCPKG_ROOT = $preferredVcpkgRoot
        Write-Host "Using vcpkg at: $preferredVcpkgRoot" -ForegroundColor Green
    }

    if (-not $env:VCPKG_ROOT) {
        Write-Warning "VCPKG_ROOT not set. Set it to your vcpkg root to locate OpenSSL/libwebsockets."
        Write-Warning "Example: `$env:VCPKG_ROOT = 'C:\dev\vcpkg'"
    }

    $toolchain = $null
    if ($env:VCPKG_ROOT -and (Test-Path "$env:VCPKG_ROOT\scripts\buildsystems\vcpkg.cmake")) {
        $toolchain = "$env:VCPKG_ROOT\scripts\buildsystems\vcpkg.cmake"
    }

    $buildDir = Join-Path $RootDir "build\windows-msvc"
    $cacheFile = Join-Path $buildDir "CMakeCache.txt"
    if ($toolchain -and (Test-Path $cacheFile)) {
        $cacheContent = Get-Content $cacheFile -Raw
        $toolchainMatches = $cacheContent -match [regex]::Escape($toolchain)
        $vcpkgRootMatches = $false
        if ($env:VCPKG_ROOT) {
            $vcpkgRootMatches = $cacheContent -match [regex]::Escape("Z_VCPKG_ROOT_DIR:INTERNAL=$env:VCPKG_ROOT")
        }
        if (-not $toolchainMatches -or (-not $vcpkgRootMatches)) {
            Write-Host "Clearing cached CMake config (vcpkg/toolchain mismatch)." -ForegroundColor Yellow
            Remove-Item $buildDir -Recurse -Force
        }
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
        if ($env:VCPKG_ROOT) {
            $cmakeArgs += "-DVCPKG_INSTALLED_DIR=$env:VCPKG_ROOT\installed"
        }
    }

    cmake @cmakeArgs
    cmake --build build/windows-msvc --config Release
    exit $LASTEXITCODE
}

cmake -S . -B build/windows -G "MinGW Makefiles" -DCMAKE_BUILD_TYPE=Release
cmake --build build/windows

