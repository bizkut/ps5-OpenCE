#!/usr/bin/env python3
"""Mesa for the PS5 build (docs/ps5_port_plan.md, D5): RADV, Zink and EGL from
../PS5_Mesa at the commit port/ps5/deps.lock pins, with this repository's
patches (port/ps5/patches/mesa), as three archives a title links:

    python tools/ps5_mesa.py           # builds them, prints where they are
    python tools/ps5_mesa.py --debug   # the same unoptimised, with Mesa's debug
                                       # messages, in build/ps5/deps/mesa-debug

    build/ps5/deps/mesa/lib/libvulkan_radeon.a   RADV with its PS5 winsys, linked whole
    build/ps5/deps/mesa/lib/libgallium.a         Zink, the DRI frontend, Mesa's GL
    build/ps5/deps/mesa/lib/libEGL.a             EGL (its surfaceless platform)
    build/ps5/deps/mesa/include/                 EGL, KHR, GLES2, GLES3 and Vulkan headers
    build/ps5/deps/mesa/PROVENANCE.txt

The source is exported with git archive and patched in build/ps5/mesa/src;
files the commit and patches did not change keep their times, so a rebuild
compiles only what changed. As ../PS5_Vulkan's tools/build-radv.sh does, the
OpenCL C kernels RADV needs are compiled by host tools of the same commit
(mesa_clc, vtn_bindgen2) built against this computer's LLVM 19, Clang,
libclc and SPIR-V translator, and the console build uses the payload SDK
of tools/ps5_deps.py and PS5_Vulkan's meson cross file.
"""

import hashlib
import io
import os
import shutil
import subprocess
import sys
import tarfile
from pathlib import Path
from typing import List

import ps5_deps

ROOT = Path(__file__).resolve().parent.parent
WORK = ROOT / "build" / "ps5" / "mesa"
INSTALL = ps5_deps.DEPS / "mesa"
LLVM_CONFIG = "llvm-config-19"

OPTIONS = [
    "-Dvulkan-drivers=amd", "-Dgallium-drivers=zink", "-Dplatforms=", "-Dradv-winsys=ps5",
    "-Dllvm=disabled", "-Damd-use-llvm=false", "-Dvideo-codecs=",
    "-Dbuildtype=debugoptimized", "-Db_ndebug=true",
    "-Dglx=disabled", "-Degl=enabled", "-Dgbm=disabled", "-Dopengl=true", "-Dgles1=disabled", "-Dgles2=enabled",
    "-Dvalgrind=disabled", "-Dlibunwind=disabled", "-Dzstd=disabled", "-Dzlib=enabled", "--force-fallback-for=zlib",
    "-Dexpat=disabled", "-Dxmlconfig=disabled", "-Dshader-cache=enabled", "-Dbuild-tests=false",
    "-Dvulkan-layers=", "-Dtools=", "-Dmesa-clc=system",
]
TARGETS = {
    "libvulkan_radeon.a": "src/amd/vulkan/libvulkan_radeon.a",
    "libEGL.a": "src/egl/libEGL.a",
    "libgallium.a": "src/gallium/targets/dri/libgallium-{version}.a",
}
HEADERS = ("EGL", "KHR", "GLES2", "GLES3", "vulkan", "vk_video")


def _run(command: List[str], log: Path, env: dict = None) -> None:
    with log.open("w", encoding="utf-8") as output:
        result = subprocess.run([str(part) for part in command], stdout=output, stderr=subprocess.STDOUT, env=env)
    if result.returncode:
        lines = log.read_text(encoding="utf-8", errors="replace").splitlines()
        failed = [line for line in lines if "FAILED" in line or "error" in line.lower()]
        sys.stderr.write("\n".join((failed or lines)[-30:]) + "\n")
        raise SystemExit(f"failed: {' '.join(map(str, command[:3]))} (log: {log})")


def _patches() -> List[Path]:
    return sorted((ROOT / ps5_deps.lock()["ps5_mesa"]["patches"]).glob("*.patch"))


def source_identity() -> str:
    """the commit and the digest of the patches applied over it"""
    digest = hashlib.sha256()
    for patch in _patches():
        digest.update(patch.name.encode() + b"\0" + patch.read_bytes())
    return f"{ps5_deps.lock()['ps5_mesa']['commit']} patches {digest.hexdigest()[:16]}"


def export_source() -> Path:
    """build/ps5/mesa/src: the pinned commit with the patches applied"""
    entry = ps5_deps.lock()["ps5_mesa"]
    source = WORK / "src"
    stamp = source / ".opence-source"
    identity = source_identity()
    if stamp.is_file() and stamp.read_text(encoding="utf-8").strip() == identity:
        return source
    repository = (ROOT / entry["repository"]).resolve()
    staging = WORK / "src.new"
    if staging.exists():
        shutil.rmtree(staging)
    staging.mkdir(parents=True)
    archive = subprocess.run(["git", "-C", str(repository), "archive", "--format=tar", entry["commit"]],
                             check=True, stdout=subprocess.PIPE).stdout
    with tarfile.open(fileobj=io.BytesIO(archive)) as tar:
        tar.extractall(staging, filter="tar")
    # (patch, not git apply: inside this repository's work tree git apply
    # skips paths outside the current directory without a word)
    for patch in _patches():
        subprocess.run(["patch", "-p1", "--forward", "--batch", "--quiet", "-i", str(patch)], cwd=staging, check=True)
    source.mkdir(parents=True, exist_ok=True)
    # by content: unchanged files keep their times; meson's downloaded
    # subprojects stay
    subprocess.run(["rsync", "-rlp", "--checksum", "--delete", "--exclude=/.opence-source",
                    "--exclude=/subprojects/packagecache/", "--exclude=/subprojects/zlib-*/",
                    f"{staging}/", f"{source}/"], check=True)
    shutil.rmtree(staging)
    stamp.write_text(identity + "\n", encoding="utf-8")
    return source


def clc_tools(source: Path) -> Path:
    """build/ps5/mesa/clc-bin: mesa_clc and vtn_bindgen2 for this computer"""
    build = WORK / "clc-build"
    tools = WORK / "clc-bin"
    meson = shutil.which("meson")
    if not meson or not shutil.which("ninja") or not shutil.which(LLVM_CONFIG):
        raise SystemExit(f"meson, ninja and {LLVM_CONFIG} (llvm-19-dev, libclang-19-dev, libclc-19-dev, "
                         "libllvmspirvlib-19-dev) are needed")
    if not (build / "build.ninja").is_file():
        native = WORK / "native-llvm.ini"
        native.write_text(f"[binaries]\nllvm-config = '{shutil.which(LLVM_CONFIG)}'\n", encoding="utf-8")
        _run([meson, "setup", build, source, "--native-file", native, "-Dbuildtype=release", "-Dmesa-clc=enabled",
              "-Dinstall-mesa-clc=true", "-Dgallium-drivers=", "-Dvulkan-drivers=", "-Dplatforms=", "-Dglx=disabled",
              "-Degl=disabled", "-Dgbm=disabled", "-Dopengl=false", "-Dgles1=disabled", "-Dgles2=disabled",
              "-Dllvm=enabled", "-Dshared-llvm=enabled", "-Dbuild-tests=false", "-Dvalgrind=disabled",
              "-Dlibunwind=disabled", "-Dzstd=disabled", "-Dxmlconfig=disabled", "-Dtools="],
             WORK / "clc-setup.log")
    _run(["ninja", "-C", build, "src/compiler/clc/mesa_clc", "src/compiler/spirv/vtn_bindgen2"], WORK / "clc-build.log")
    tools.mkdir(exist_ok=True)
    for name, path in (("mesa_clc", "clc/mesa_clc"), ("vtn_bindgen2", "spirv/vtn_bindgen2")):
        link = tools / name
        if link.is_symlink() or link.exists():
            link.unlink()
        link.symlink_to(build / "src" / "compiler" / path)
    return tools


def build(debug: bool = False) -> Path:
    """the archives and headers in build/ps5/deps/mesa; with debug, a build
    with Mesa's assertions and its debug messages (MESA_DEBUG: debug_printf),
    unoptimised, in build/ps5/deps/mesa-debug, for finding why something
    fails on the console"""
    install = INSTALL.with_name("mesa-debug") if debug else INSTALL
    build_options = [option for option in OPTIONS if not option.startswith(("-Dbuildtype=", "-Db_ndebug="))] \
        + ["-Dbuildtype=debug", "-Db_ndebug=false"] if debug else OPTIONS
    identity = source_identity()
    sdk = ps5_deps.payload_sdk()
    sdk_revision = (sdk / ".ps5-sdk-revision").read_text(encoding="utf-8").strip()
    provenance = install / "PROVENANCE.txt"
    wanted = f"source: {identity}\nsdk: {sdk_revision}\noptions: {' '.join(build_options)}\n"
    if provenance.is_file() and provenance.read_text(encoding="utf-8") == wanted:
        return install
    source = export_source()
    env = {**os.environ, "PATH": f"{clc_tools(source)}:{os.environ['PATH']}"}
    cross = ps5_deps.title_tooling() / "radv" / "ps5-cross.ini"
    constants = WORK / "cross-constants.ini"
    constants.write_text(f"[constants]\nsdk = '{sdk}'\n", encoding="utf-8")
    build_dir = WORK / ("build-ps5-debug" if debug else "build-ps5")
    options = build_options + [f"-Dradv-build-id={ps5_deps.lock()['ps5_mesa']['commit']}"]
    options_stamp = build_dir / ".opence-options"
    if not (build_dir / "build.ninja").is_file() or \
            not options_stamp.is_file() or options_stamp.read_text(encoding="utf-8") != " ".join(options):
        wipe = ["--wipe"] if (build_dir / "build.ninja").is_file() else []
        _run(["meson", "setup", *wipe, build_dir, source, "--cross-file", constants, "--cross-file", cross, *options],
             build_dir.with_suffix(".setup.log"), env)
        options_stamp.write_text(" ".join(options), encoding="utf-8")
    version = (source / "VERSION").read_text(encoding="utf-8").strip()
    targets = {name: path.format(version=version) for name, path in TARGETS.items()}
    _run(["ninja", "-C", build_dir, *targets.values()], build_dir.with_suffix(".log"), env)

    if install.exists():
        shutil.rmtree(install)
    (install / "lib").mkdir(parents=True)
    for name, path in targets.items():
        shutil.copyfile(build_dir / path, install / "lib" / name)
    for header in HEADERS:
        if (source / "include" / header).is_dir():
            shutil.copytree(source / "include" / header, install / "include" / header)
    provenance.write_text(wanted, encoding="utf-8")
    return install


def main() -> int:
    print(f"mesa={build(debug='--debug' in sys.argv[1:])}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
