#!/usr/bin/env python3

from __future__ import annotations

import argparse
import shutil
import subprocess
import tarfile
from pathlib import Path


PRODUCT = "daptor"
RUNTIME_PATTERNS = {
    "linux": ["*.so", "*.so.*"],
    "macos": ["*.dylib"],
}


def normalized_version(raw: str) -> str:
    return raw[1:] if raw.startswith("v") else raw


def copy_runtime_files(binary_path: Path, stage_bin_dir: Path, platform: str) -> None:
    for pattern in RUNTIME_PATTERNS.get(platform, []):
        for runtime_path in binary_path.parent.glob(pattern):
            if runtime_path.is_file():
                shutil.copy2(runtime_path, stage_bin_dir / runtime_path.name)


def copy_tree(source: Path, target: Path) -> None:
    if target.exists():
        shutil.rmtree(target)
    shutil.copytree(source, target)


def bundle_linux_libraries(binary_path: Path, lib_dir: Path, repo_root: Path) -> None:
    script = repo_root / "scripts" / "ci" / "bundle_linux_libs.py"
    subprocess.run(
        [
            "python3",
            str(script),
            "--binary",
            str(binary_path),
            "--lib-dir",
            str(lib_dir),
        ],
        check=True,
    )


def stage_release_tree(
    stage_root: Path,
    ui_binary: Path,
    cli_binary: Path,
    repo_root: Path,
    platform: str,
) -> None:
    stage_bin_dir = stage_root / "bin"
    stage_bin_dir.mkdir(parents=True, exist_ok=True)

    shutil.copy2(ui_binary, stage_bin_dir / PRODUCT)
    shutil.copy2(cli_binary, stage_bin_dir / "daptor-cli")
    copy_runtime_files(ui_binary, stage_bin_dir, platform)
    copy_tree(repo_root / "config", stage_root / "config")
    copy_tree(repo_root / "schemas", stage_root / "schemas")

    readme = repo_root / "README.md"
    if readme.is_file():
        shutil.copy2(readme, stage_root / "README.md")

    if platform == "linux":
        lib_dir = stage_root / "lib"
        bundle_linux_libraries(stage_bin_dir / PRODUCT, lib_dir, repo_root)
        bundle_linux_libraries(stage_bin_dir / "daptor-cli", lib_dir, repo_root)


def create_archive(stage_dir: Path, output_dir: Path, archive_base: str) -> Path:
    output_dir.mkdir(parents=True, exist_ok=True)
    archive_path = output_dir / f"{archive_base}.tar.gz"
    with tarfile.open(archive_path, "w:gz") as archive:
        archive.add(stage_dir, arcname=stage_dir.name)
    return archive_path


def main() -> int:
    parser = argparse.ArgumentParser(description="Package daptor release archive")
    parser.add_argument("--version", required=True)
    parser.add_argument("--platform", required=True, choices=["linux", "macos"])
    parser.add_argument("--arch", required=True, choices=["x86_64", "aarch64"])
    parser.add_argument("--ui-binary", required=True)
    parser.add_argument("--cli-binary", required=True)
    parser.add_argument("--output-dir", required=True)
    args = parser.parse_args()

    version = normalized_version(args.version)
    ui_binary = Path(args.ui_binary).resolve()
    cli_binary = Path(args.cli_binary).resolve()
    if not ui_binary.is_file():
        raise FileNotFoundError(f"UI binary not found: {ui_binary}")
    if not cli_binary.is_file():
        raise FileNotFoundError(f"CLI binary not found: {cli_binary}")

    repo_root = Path(__file__).resolve().parents[2]
    output_dir = (repo_root / args.output_dir).resolve()
    archive_base = f"{PRODUCT}-{version}-{args.platform}-{args.arch}"
    stage_root = output_dir / archive_base
    if stage_root.exists():
        shutil.rmtree(stage_root)

    stage_release_tree(stage_root, ui_binary, cli_binary, repo_root, args.platform)
    archive_path = create_archive(stage_root, output_dir, archive_base)
    print(archive_path)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
