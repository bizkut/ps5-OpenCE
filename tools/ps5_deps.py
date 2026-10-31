#!/usr/bin/env python3
"""The PS5 build's dependencies (docs/ps5_port_plan.md), taken from the
repositories beside this one (../PS5_Vulkan, ../PS5_Mesa and the payload SDK
of ../forever) at the commits port/ps5/deps.lock pins:

    python tools/ps5_deps.py        # installs them, prints where they are

Each repository is exported with git archive at its pinned commit into
build/ps5/deps/src/<name>, never built in its working tree, which other work
shares. From those exports:

- the payload SDK (PS5_PayloadSDK): its own setup script installs the
  upstream v0.42 release with the fork's headers, platform layer, libc, CRT
  and stubs over it, into build/ps5/deps/ps5-payload-sdk;
- PS5_Vulkan's title tooling: the compiler wrapper (prospero-clang18), the
  CRT the title starts with, the link script, the ELF to SCE module writer
  and SELF signer (built here in build/ps5/deps/host/<commit>/), and
  the libc.prx a title carries in sce_module/, generated from the recorded
  manifests by its libc_builder and signed, as its tools/rebuild-libc.sh
  does, then checked against both digests that script records.

A dependency already installed at its pinned commit is left as it is.
"""

import hashlib
import io
import json
import shutil
import subprocess
import sys
import tarfile
from pathlib import Path
from typing import Any, Dict, List

ROOT = Path(__file__).resolve().parent.parent
LOCK = ROOT / "port" / "ps5" / "deps.lock"
DEPS = ROOT / "build" / "ps5" / "deps"
STAMP = ".opence-commit"


def lock() -> Dict[str, Any]:
    return json.loads(LOCK.read_text(encoding="utf-8"))


def _repository(entry: Dict[str, Any]) -> Path:
    repository = (ROOT / entry["repository"]).resolve()
    if not (repository / ".git").exists():
        raise SystemExit(f"{repository} is not a git repository (port/ps5/deps.lock: {entry['repository']})")
    return repository


def _sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as file:
        for block in iter(lambda: file.read(1 << 20), b""):
            digest.update(block)
    return digest.hexdigest()


def export(name: str) -> Path:
    """the repository of deps.lock entry `name` at its commit (only its
    `paths`, if the entry lists them), in build/ps5/deps/src/<name>"""
    entry = lock()[name]
    destination = DEPS / "src" / name
    stamp = destination / STAMP
    wanted = " ".join([entry["commit"], *entry.get("paths", [])])
    if stamp.is_file() and stamp.read_text(encoding="utf-8").strip() == wanted:
        return destination
    repository = _repository(entry)
    archive = subprocess.run(
        ["git", "-C", str(repository), "archive", "--format=tar", entry["commit"], *entry.get("paths", [])],
        check=True, stdout=subprocess.PIPE).stdout
    if destination.exists():
        shutil.rmtree(destination)
    destination.mkdir(parents=True)
    with tarfile.open(fileobj=io.BytesIO(archive)) as tar:
        tar.extractall(destination, filter="tar")
    stamp.write_text(wanted + "\n", encoding="utf-8")
    return destination


def payload_sdk() -> Path:
    """the installed payload SDK (build/ps5/deps/ps5-payload-sdk)"""
    entry = lock()["payload_sdk"]
    sdk = DEPS / "ps5-payload-sdk"
    revision = sdk / ".ps5-sdk-revision"
    if revision.is_file() and revision.read_text(encoding="utf-8").strip() == entry["commit"]:
        return sdk
    tree = export("payload_sdk")
    # setup-sdk.sh takes the release from its cache when the digest matches,
    # and would download it otherwise: give it the copy beside the SDK
    cache = DEPS / "cache"
    cache.mkdir(parents=True, exist_ok=True)
    release = (ROOT / entry["release"]).resolve()
    cached = cache / "ps5-payload-sdk.zip"
    if release.is_file() and not cached.is_file():
        if _sha256(release) != entry["release_sha256"]:
            raise SystemExit(f"{release}: SHA-256 differs from port/ps5/deps.lock")
        shutil.copyfile(release, cached)
    subprocess.run(["bash", str(tree / "platform" / "tools" / "setup-sdk.sh"), str(sdk), entry["commit"], str(cache)],
                   check=True)
    return sdk


def title_tooling() -> Path:
    """PS5_Vulkan's title tooling (tooling/)"""
    return export("ps5_vulkan") / "tooling"


def _host_compiler() -> str:
    compiler = shutil.which("clang++") or shutil.which("clang++-18")
    if not compiler:
        raise SystemExit("clang++ was not found")
    return compiler


def _host_program(name: str, sources: List[Path], libraries: List[str]) -> Path:
    """build/ps5/deps/host/<commit>/<name>: built once for each pinned
    PS5_Vulkan commit (git archive gives the sources the commit's time, which
    can be older than a program built from another commit)"""
    program = DEPS / "host" / lock()["ps5_vulkan"]["commit"][:12] / name
    if program.is_file():
        return program
    program.parent.mkdir(parents=True, exist_ok=True)
    subprocess.run([_host_compiler(), "-std=c++20", "-O2", "-Wall", "-Wextra", "-Werror",
                    *map(str, sources), *libraries, "-o", str(program)], check=True)
    return program


def native_tool() -> Path:
    """build/ps5/deps/host/<commit>/ps5-native-tool: PS5_Vulkan's ELF to SCE module
    writer and SELF signer, built for this computer"""
    native = title_tooling() / "native"
    return _host_program("ps5-native-tool", [native / name for name in (
        "native_app_builder.cpp", "self_container.cpp", "elf_object.cpp", "sce_module_writer.cpp")], ["-lz"])


def libc_prx() -> Path:
    """build/ps5/deps/libc.prx, the module a title carries as sce_module/libc.prx"""
    entry = lock()["ps5_vulkan"]
    signed = DEPS / "libc.prx"
    if signed.is_file() and _sha256(signed) == entry["libc_prx_sha256"]:
        return signed
    native = title_tooling() / "native"
    builder = _host_program("libc-builder", [native / "libc_builder.cpp"], [])
    raw = DEPS / "host" / "libc.raw.elf"
    subprocess.run([str(builder), str(native / "runtime" / "api-surface.txt"),
                    str(native / "runtime" / "imports.txt"), str(raw)], check=True)
    if _sha256(raw) != entry["libc_raw_sha256"]:
        raise SystemExit(f"{raw}: SHA-256 differs from port/ps5/deps.lock")
    subprocess.run([str(native_tool()), "self", "--sign", "--in", str(raw), "--out", str(signed)], check=True)
    if _sha256(signed) != entry["libc_prx_sha256"]:
        signed.unlink()
        raise SystemExit("the signed libc.prx's SHA-256 differs from port/ps5/deps.lock")
    return signed




def main() -> int:
    print(f"sdk={payload_sdk()}")
    print(f"tooling={title_tooling()}")
    print(f"native_tool={native_tool()}")
    print(f"libc_prx={libc_prx()}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
