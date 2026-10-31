"""Ninja graph for the x32 guest shared by the Linux and PS5 hosts."""

import json
import shutil
from pathlib import Path

from .android_build import LINUX_DIR, SDL_DIR, fetch_third_party, generate_guest_build
from .ps5_host_build import generate_ps5_linux_host

BUILD = Path("build/ps5")
GUEST_ABI_FLAGS = [
    "--target=x86_64-linux-gnux32", "-march=x86-64-v3", "-ffp-contract=off", "-femulated-tls", "-fno-pic",
    "-DHALO_GUEST=1", "-DHALO_X32_GUEST=1", "-DHALO_GLES=1", "-D__linux__=1", "-nostdinc", "-fshort-wchar",
    "-fno-stack-protector", "-fno-unwind-tables", "-fno-asynchronous-unwind-tables", "-O2",
]


def generate_ps5_build(n, sln):
    if not shutil.which("clang"):
        n.comment("PS5 guest build: clang not found")
        return
    fetch_third_party()
    config = json.loads((LINUX_DIR / "port.json").read_text(encoding="utf-8"))
    guest_cc = "clang"
    gl_include = SDL_DIR / "src/video/khronos"
    n.variable("ps5_guest_cc", guest_cc)
    image, host_table_c = generate_guest_build(
        n, sln, config, name="ps5", build=BUILD, guest_cc=guest_cc,
        sysroot_include=gl_include, gl_include=gl_include, arch=Path("port/ps5/guest/libc/arch/x32"),
        abi_flags=GUEST_ABI_FLAGS)
    host = generate_ps5_linux_host(n, sln, image, host_table_c)
    n.build("ps5_guest", "phony", image)
    n.build("ps5_linux_host", "phony", [image, host])
    n.newline()
