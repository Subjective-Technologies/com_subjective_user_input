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
    chosen_gen = generator or os.environ.get("CMAKE_GENERATOR") or cached_gen

    args = ["cmake", "-S", str(ROOT), "-B", str(build_dir)]

    if platform_name == "windows":
        default_gen = "Visual Studio 17 2022"
        # Prefer a newer VS if the cache had one
        gen = chosen_gen or cached_gen or default_gen
        if cached_gen and cached_gen != gen:
            log(f"[INFO] Generator changed (was '{cached_gen}', now '{gen}'); clearing {build_dir}")
            import shutil
            shutil.rmtree(build_dir)
            build_dir.mkdir(parents=True, exist_ok=True)
        args += ["-G", gen, "-A", "x64"]
        vcpkg = os.environ.get("VCPKG_INSTALLATION_ROOT")
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
