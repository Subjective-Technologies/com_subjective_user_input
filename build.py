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
from pathlib import Path

ROOT = Path(__file__).resolve().parent
BUILD_ROOT = ROOT / "build"


def log(msg: str) -> None:
    print(msg, flush=True)


def run(cmd: list[str]) -> None:
    log("+ " + " ".join(str(c) for c in cmd))
    subprocess.run(cmd, check=True)


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

    args = ["cmake", "-S", str(ROOT), "-B", str(build_dir)]

    if platform_name == "windows":
        gen = generator or "Visual Studio 17 2022"
        args += ["-G", gen, "-A", "x64"]
        vcpkg = sys.environ.get("VCPKG_INSTALLATION_ROOT")
        if vcpkg:
            args += [f"-DCMAKE_TOOLCHAIN_FILE={Path(vcpkg) / 'scripts' / 'buildsystems' / 'vcpkg.cmake'}"]
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
