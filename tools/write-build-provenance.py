#!/usr/bin/env python3
# ps5-native-app-boilerplate / ProsperoLight - Build receipt outside the installed app.
# Copyright (C) 2026 BlackBearReloaded
# SPDX-License-Identifier: GPL-3.0-or-later
import hashlib
import json
import os
from pathlib import Path
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]


def run(*args):
    return subprocess.check_output(args, cwd=ROOT, text=True).strip()


def digest(path):
    with Path(path).open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest()


def git_value(*args):
    try:
        return subprocess.check_output(("git", *args), cwd=ROOT, text=True,
                                       stderr=subprocess.DEVNULL).strip()
    except (OSError, subprocess.CalledProcessError):
        return ""


def source_files():
    for entry in ("src", "include", "tools", "tooling", "third_party", "assets", "sce_sys"):
        for path in (ROOT / entry).rglob("*"):
            if path.is_file() and not path.name.startswith("._") and "__pycache__" not in path.parts:
                yield path
    for entry in ("Makefile", "README.md", "NOTICE.md"):
        path = ROOT / entry
        if path.is_file():
            yield path


def main():
    executable, destination = map(Path, sys.argv[1:])
    sdk = ROOT / ".deps/native/ps5-payload-sdk"
    llvm = os.environ["LLVM_CONFIG"]
    bindir = Path(run(llvm, "--bindir"))
    # Exact source contents, including local work, also in a copied source tree.
    source_hash = hashlib.sha256()
    for path in sorted(source_files()):
        name = str(path.relative_to(ROOT))
        source_hash.update(name.encode() + b"\0")
        source_hash.update(digest(path).encode())
    inputs = [ROOT / p for p in os.environ.get("APP_STATIC_ARCHIVES", "").split()]
    inputs += [ROOT / "build/runtime-shim/libc.prx", ROOT / "tools/setup-native-dependencies.sh"]
    inputs += sorted((sdk / "target/lib").glob("*.so"))
    receipt = {
        "schema": 1,
        "commit": git_value("rev-parse", "HEAD") or "unavailable",
        "source_sha256": source_hash.hexdigest(),
        "submodules": git_value("submodule", "status", "--recursive").splitlines(),
        "definitions": os.environ.get("APP_DEFINITIONS", "").split(),
        "llvm_version": run(llvm, "--version"),
        "compiler_version": run(os.environ["PS5_CLANG"], "--version").splitlines()[0],
        "app_compiler_sha256": digest(os.environ["PS5_CLANG"]),
        "sdk_compiler_version": run(str(sdk / "bin/prospero-clang"), "--version").splitlines()[0],
        "tool_sha256": {name: digest(bindir / name) for name in ("clang", "ld.lld", "llvm-ar")},
        "input_sha256": {str(p.relative_to(ROOT)): digest(p) for p in inputs},
        "eboot_sha256": digest(executable),
    }
    destination.write_text(json.dumps(receipt, indent=2) + "\n", encoding="utf-8")
    print(f"Build provenance: {destination}")


if __name__ == "__main__":
    main()
