#!/usr/bin/env python3

from __future__ import annotations

import argparse
import re
import shutil
import subprocess
from pathlib import Path

SYSTEM_LIB_PREFIXES = (
    "ld-linux",
    "linux-vdso",
)
SYSTEM_LIB_NAMES = {
    "libc.so.6",
    "libm.so.6",
    "libpthread.so.0",
    "libdl.so.2",
    "librt.so.1",
    "libresolv.so.2",
    "libgcc_s.so.1",
    "libstdc++.so.6",
    "libutil.so.1",
}


def should_skip(soname: str) -> bool:
    if soname in SYSTEM_LIB_NAMES:
        return True
    return any(soname.startswith(prefix) for prefix in SYSTEM_LIB_PREFIXES)


def parse_ldd(binary: Path) -> list[tuple[str, Path]]:
    output = subprocess.check_output(["ldd", str(binary)], text=True, stderr=subprocess.STDOUT)
    deps: list[tuple[str, Path]] = []
    for line in output.splitlines():
        match = re.match(r"^\s*(\S+)\s+=>\s+(\S+)\s+\(", line)
        if not match:
            continue
        soname, resolved = match.group(1), match.group(2)
        if resolved == "not" or should_skip(soname):
            continue
        deps.append((soname, Path(resolved)))
    return deps


def collect_dependencies(binary: Path) -> dict[str, Path]:
    pending = [binary]
    seen_binaries: set[Path] = set()
    collected: dict[str, Path] = {}

    while pending:
        current = pending.pop()
        current = current.resolve()
        if current in seen_binaries:
            continue
        seen_binaries.add(current)

        for soname, resolved in parse_ldd(current):
            if soname in collected:
                continue
            collected[soname] = resolved.resolve()
            if resolved.suffix == ".so" or ".so." in resolved.name:
                pending.append(resolved)

    return collected


def copy_libraries(deps: dict[str, Path], lib_dir: Path) -> list[Path]:
    lib_dir.mkdir(parents=True, exist_ok=True)
    copied: list[Path] = []
    for soname, source in sorted(deps.items()):
        target = lib_dir / soname
        shutil.copy2(source, target, follow_symlinks=True)
        copied.append(target)
    return copied


def set_rpath(target: Path, rpath: str) -> None:
    subprocess.run(
        ["patchelf", "--set-rpath", rpath, str(target)],
        check=True,
        stdout=subprocess.DEVNULL,
        stderr=subprocess.PIPE,
        text=True,
    )


def bundle_binary(binary: Path, lib_dir: Path) -> list[Path]:
    deps = collect_dependencies(binary)
    copied = copy_libraries(deps, lib_dir)
    set_rpath(binary, "$ORIGIN/../lib")
    for library in copied:
        set_rpath(library, "$ORIGIN")
    return copied


def main() -> int:
    parser = argparse.ArgumentParser(description="Bundle non-glibc shared libraries for Linux daptor")
    parser.add_argument("--binary", required=True, help="Path to daptor executable")
    parser.add_argument("--lib-dir", required=True, help="Directory to place bundled .so files")
    args = parser.parse_args()

    binary = Path(args.binary).resolve()
    lib_dir = Path(args.lib_dir).resolve()
    if not binary.is_file():
        raise FileNotFoundError(f"binary not found: {binary}")

    copied = bundle_binary(binary, lib_dir)
    print(f"bundled {len(copied)} libraries into {lib_dir}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
