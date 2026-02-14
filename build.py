#!/usr/bin/env python3
"""
Unified build script for subjective_user_input_c (KVM tool).

Usage:
  python build.py [--config Release|Debug] [--clean] [--generator NAME] [--skip-deps]
"""
from __future__ import annotations

import argparse
import platform
import shutil
import subprocess
import sys
import os
from pathlib import Path

ROOT = Path(__file__).resolve().parent
BUILD_ROOT = ROOT / "build"


def log(msg: str) -> None:
    print(msg, flush=True)


def run(cmd: list[str]) -> None:
    log("+ " + " ".join(str(c) for c in cmd))
    subprocess.run(cmd, check=True)


def load_root_env() -> None:
    env_path = ROOT.parent.parent / ".env"
    if not env_path.exists():
        return
    loaded = 0
    for line in env_path.read_text(encoding="utf-8").splitlines():
        line = line.strip()
        if not line or line.startswith("#") or "=" not in line:
            continue
        k, v = line.split("=", 1)
        k = k.strip()
        v = v.strip().strip('"').strip("'")
        if k and v and k not in os.environ:
            os.environ[k] = v
            loaded += 1
    if loaded:
        log(f"[INFO] Loaded {loaded} vars from {env_path}")


def cached_generator(build_dir: Path) -> str | None:
    cache = build_dir / "CMakeCache.txt"
    if not cache.exists():
        return None
    for line in cache.read_text(errors="ignore").splitlines():
        if line.startswith("CMAKE_GENERATOR:"):
            parts = line.split("=", 1)
            if len(parts) == 2:
                return parts[1].strip()
    return None


def cached_toolchain(build_dir: Path) -> Path | None:
    cache = build_dir / "CMakeCache.txt"
    if not cache.exists():
        return None
    for line in cache.read_text(errors="ignore").splitlines():
        if line.startswith("CMAKE_TOOLCHAIN_FILE:"):
            parts = line.split("=", 1)
            if len(parts) == 2:
                return Path(parts[1].strip())
    return None


def detect_vs_generator() -> str | None:
    """Pick a Visual Studio generator based on installed instances."""
    program_files_x86 = os.environ.get("ProgramFiles(x86)")
    if program_files_x86:
        vswhere = Path(program_files_x86) / "Microsoft Visual Studio" / "Installer" / "vswhere.exe"
        if vswhere.exists():
            try:
                result = subprocess.run(
                    [
                        str(vswhere),
                        "-latest",
                        "-products",
                        "*",
                        "-requires",
                        "Microsoft.Component.MSBuild",
                        "-property",
                        "installationVersion",
                    ],
                    capture_output=True,
                    text=True,
                    check=True,
                )
                version = (result.stdout or "").strip()
                if version.startswith("17."):
                    return "Visual Studio 17 2022"
                if version.startswith("18."):
                    return "Visual Studio 18 2026"
            except Exception:
                pass

    # Stable fallback for GitHub-hosted runners.
    return "Visual Studio 17 2022"


def detect_platform() -> str:
    sys_name = platform.system().lower()
    if "windows" in sys_name:
        return "windows"
    if "darwin" in sys_name:
        return "macos"
    if "linux" in sys_name:
        return "linux"
    raise SystemExit(f"Unsupported platform: {sys_name}")


def configure(platform_name: str, cfg: str, generator: str | None) -> Path:
    build_dir = BUILD_ROOT / platform_name
    build_dir.mkdir(parents=True, exist_ok=True)

    cached_gen = cached_generator(build_dir)
    cached_tc = cached_toolchain(build_dir)
    chosen_gen = generator or os.environ.get("CMAKE_GENERATOR") or cached_gen

    args = ["cmake", "-S", str(ROOT), "-B", str(build_dir)]

    if platform_name == "windows":
        detected = detect_vs_generator()
        default_gen = detected or ("Ninja" if shutil.which("ninja") else "Visual Studio 17 2022")
        # Prefer a newer VS if the cache had one
        gen = chosen_gen or cached_gen or default_gen
        if cached_gen and cached_gen != gen:
            log(f"[INFO] Generator changed (was '{cached_gen}', now '{gen}'); clearing {build_dir}")
            shutil.rmtree(build_dir)
            build_dir.mkdir(parents=True, exist_ok=True)
            cached_tc = None
        args += ["-G", gen]
        if gen.startswith("Visual Studio"):
            args += ["-A", "x64"]
        vcpkg_root = os.environ.get("VCPKG_INSTALLATION_ROOT")
        toolchain_env = os.environ.get("VCPKG_TOOLCHAIN_FILE")
        toolchain = None
        if toolchain_env:
            toolchain = Path(toolchain_env)
        elif vcpkg_root:
            toolchain = Path(vcpkg_root) / "scripts" / "buildsystems" / "vcpkg.cmake"
        if cached_tc and not cached_tc.exists():
            log(f"[WARN] Cached toolchain missing ({cached_tc}); clearing {build_dir}")
            shutil.rmtree(build_dir)
            build_dir.mkdir(parents=True, exist_ok=True)
            cached_tc = None
        if toolchain and toolchain.exists():
            args += [f"-DCMAKE_TOOLCHAIN_FILE={toolchain}"]
            log(f"[INFO] Using vcpkg toolchain: {toolchain}")
        elif toolchain:
            log(f"[WARN] VCPKG toolchain not found at {toolchain}. Continuing without vcpkg.")

        # Pass OpenSSL hints if provided in .env
        openssl_root = os.environ.get("OPENSSL_ROOT_DIR")
        openssl_inc = os.environ.get("OPENSSL_INCLUDE_DIR")
        openssl_crypto = os.environ.get("OPENSSL_CRYPTO_LIBRARY")
        openssl_ssl = os.environ.get("OPENSSL_SSL_LIBRARY")
        if openssl_root:
            args += [f"-DOPENSSL_ROOT_DIR={openssl_root}"]
        if openssl_inc:
            args += [f"-DOPENSSL_INCLUDE_DIR={openssl_inc}"]
        if openssl_crypto:
            args += [f"-DOPENSSL_CRYPTO_LIBRARY={openssl_crypto}"]
        if openssl_ssl:
            args += [f"-DOPENSSL_SSL_LIBRARY={openssl_ssl}"]

        if not toolchain and not (openssl_root or openssl_inc or openssl_crypto or openssl_ssl):
            log("[ERROR] OpenSSL not found and no vcpkg toolchain set. Set OPENSSL_ROOT_DIR or install vcpkg and set VCPKG_INSTALLATION_ROOT/VCPKG_TOOLCHAIN_FILE.")
            raise SystemExit(1)
        args += [
            f"-DCMAKE_RUNTIME_OUTPUT_DIRECTORY={build_dir / 'bin'}",
            f"-DCMAKE_RUNTIME_OUTPUT_DIRECTORY_RELEASE={build_dir / 'bin' / 'Release'}",
            f"-DCMAKE_LIBRARY_OUTPUT_DIRECTORY_RELEASE={build_dir / 'bin' / 'Release'}",
            f"-DCMAKE_ARCHIVE_OUTPUT_DIRECTORY_RELEASE={build_dir / 'lib' / 'Release'}",
        ]
    else:
        gen = generator or ("Ninja" if shutil.which("ninja") else "Unix Makefiles")
        args += ["-G", gen, f"-DCMAKE_BUILD_TYPE={cfg}"]
        args += [
            f"-DCMAKE_RUNTIME_OUTPUT_DIRECTORY={build_dir / 'bin'}",
            f"-DCMAKE_LIBRARY_OUTPUT_DIRECTORY={build_dir / 'lib'}",
            f"-DCMAKE_ARCHIVE_OUTPUT_DIRECTORY={build_dir / 'lib'}",
        ]

    run(args)
    return build_dir


def build(build_dir: Path, platform_name: str, cfg: str) -> None:
    cmd = ["cmake", "--build", str(build_dir), "--parallel"]
    if platform_name == "windows":
        cmd += ["--config", cfg]
    run(cmd)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--config", default="Release", choices=["Release", "Debug"])
    parser.add_argument("--clean", action="store_true")
    parser.add_argument("--generator", help="Override CMake generator")
    parser.add_argument("--skip-deps", action="store_true", help="Reserved for future dependency installation")
    args = parser.parse_args()

    load_root_env()
    plat = detect_platform()
    build_dir = BUILD_ROOT / plat

    if args.clean and build_dir.exists():
        shutil.rmtree(build_dir)

    if not args.skip_deps:
        log("[INFO] Skipping dependency installation (handled externally).")

    cfg_dir = configure(plat, args.config, args.generator)
    build(cfg_dir, plat, args.config)
    log(f"[OK] Build finished for {plat} ({args.config}).")
    return 0


if __name__ == "__main__":
    sys.exit(main())
