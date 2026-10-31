#!/usr/bin/env python3
"""Builds OpenCE's M0 probes (port/ps5/probe, docs/ps5_port_plan.md):

    python tools/ps5_probe.py linux [--run]   # build/ps5/probe-linux: S1-S4 on this computer
    python tools/ps5_probe.py ps5             # build/ps5/probe/PPSA99117: S1-S4's title folder
    python tools/ps5_probe.py s5a             # build/ps5/s5a/PPSA99117: S5a's (Zink on RADV, readback)
    python tools/ps5_probe.py s5b             # build/ps5/s5b/PPSA99117: S5b's (Zink presenting itself)
    python tools/ps5_probe.py s6              # build/ps5/s6/PPSA99117: S6's (S5b with the pad, keyboard, mouse, tone)
    python tools/ps5_probe.py s78             # build/ps5/s78/PPSA99117: S7's and S8's (sockets, /data/opence)
    python tools/ps5_probe.py s7-peer         # S7's other end, on this computer, until interrupted

S1-S4's builds carry the same x32 guest image (probe_guest.elf), linked at
0x7c000000 with this computer's clang and ld.lld. The Linux build checks the
probe's own logic (the loader, the stubs, the threads, the fault handler)
before a console runs it; --run runs it with a two-second S3. S5a, S5b and
S6 link RADV, Zink and EGL from tools/ps5_mesa.py (--debug-mesa: its debug
build).
S7's other end, s7-peer, sends the console (PS5_HOST, 10.0.1.41 by
default) a datagram on the game's port 2302 every half second and connects
to its TCP port 2302, confirming each answer, until interrupted.
"""

import argparse
import json
import os
import shutil
import socket
import subprocess
import sys
import threading
import time
from pathlib import Path
from typing import List

import ps5_title

ROOT = Path(__file__).resolve().parent.parent
PROBE = ROOT / "port" / "ps5" / "probe"
BUILD = ROOT / "build" / "ps5"
GAME_PORT = 2302

# the guest's code generation (docs/ps5_port_plan.md D1, D9): x32, the PS5's
# instruction set (Zen 2 has x86-64-v3), no fused multiply-add
GUEST_FLAGS = ["--target=x86_64-linux-gnux32", "-march=x86-64-v3", "-ffp-contract=off", "-O2", "-g",
               "-ffreestanding", "-fno-builtin", "-fno-pic", "-fno-stack-protector", "-fno-omit-frame-pointer",
               "-fno-asynchronous-unwind-tables", "-Wall", "-Wextra", "-Werror"]
HOST_WARNINGS = ["-Wall", "-Wextra", "-Werror"]


def _tool(*names: str) -> str:
    for name in names:
        found = shutil.which(name)
        if found:
            return found
    raise SystemExit(f"{names[0]} was not found")


def build_guest(output: Path) -> Path:
    clang = _tool("clang", "clang-18")
    objects: List[str] = []
    output.mkdir(parents=True, exist_ok=True)
    for source in ("probe_guest.c", "probe_work.c", "probe_guest_stubs.S"):
        target = output / ("guest_" + Path(source).stem + ".o")
        subprocess.run([clang, *GUEST_FLAGS, "-c", str(PROBE / source), "-o", str(target)], check=True)
        objects.append(str(target))
    image = output / "probe_guest.elf"
    subprocess.run([_tool("ld.lld", "ld.lld-18"), "-m", "elf32_x86_64", "-static", "-nostdlib", "--no-dynamic-linker",
                    "-T", str(PROBE / "probe_guest.ld"), "-o", str(image), *objects], check=True)
    return image


def build_linux(run: bool) -> int:
    output = BUILD / "probe-linux"
    build_guest(output)
    program = output / "opence-probe"
    subprocess.run([_tool("clang", "clang-18"), "-O2", "-g", "-pthread", "-ffp-contract=off", "-march=x86-64-v3",
                    *HOST_WARNINGS, *(str(PROBE / name) for name in ("probe.c", "probe_work.c", "probe_host_call.S")),
                    "-o", str(program)], check=True)
    print(program)
    if not run:
        return 0
    return subprocess.run([str(program)], cwd=output, env={**os.environ, "PROBE_SPIN_SECONDS": "2"}).returncode


def build_ps5() -> int:
    toolchain = ps5_title.Toolchain()
    build = BUILD / "probe"
    image = build_guest(build)
    flags = ["-std=gnu11", "-O2", "-g", "-ffp-contract=off", "-fno-omit-frame-pointer", "-ffunction-sections",
             "-fdata-sections", *HOST_WARNINGS]
    objects = [toolchain.compile(PROBE / name, build / "obj" / (name + ".o"), flags)
               for name in ("probe.c", "probe_work.c", "probe_host_call.S")]
    module = toolchain.link(objects, build)
    param = json.loads((ROOT / "port" / "ps5" / "sce_sys" / "param.json").read_text(encoding="utf-8"))
    param["localizedParameters"]["en-US"]["titleName"] = "OpenCE M0 probe"
    param_path = build / "param.json"
    param_path.write_text(json.dumps(param, indent=2) + "\n", encoding="utf-8")
    icon = ps5_title.plain_icon(build / "icon0.png", (0x3a, 0x5f, 0x2b))
    folder = toolchain.stage(module, build / param["titleId"], param_path, icon, [image])
    print(folder)
    return 0


def build_s5(probe: str, debug: bool) -> int:
    """S5a, S5b or S6: OpenGL ES through Zink on RADV (port/ps5/probe/s5_zink.c;
    S5b with PROBE_S5B; S6 with PROBE_S6 too and s6_devices.c), with Mesa
    from tools/ps5_mesa.py (with debug, its debug build)"""
    import ps5_mesa

    mesa = ps5_mesa.build(debug=debug)
    toolchain = ps5_title.Toolchain()
    build = BUILD / probe
    flags = ["-std=gnu11", "-O2", "-g", "-fno-omit-frame-pointer", "-ffunction-sections", "-fdata-sections",
             "-I", str(mesa / "include"), "-I", str(PROBE), *HOST_WARNINGS]
    if probe in ("s5b", "s6"):
        flags.append("-DPROBE_S5B")
    if probe == "s6":
        flags.append("-DPROBE_S6")
    sources = ["s5_zink.c", *(["s6_devices.c"] if probe == "s6" else [])]
    objects = [toolchain.compile(PROBE / name, build / "obj" / (name + ".o"), flags) for name in sources]
    module = toolchain.link(objects, build, radv_archive=mesa / "lib" / "libvulkan_radeon.a",
                            archives=[mesa / "lib" / "libgallium.a", mesa / "lib" / "libEGL.a"])
    param = json.loads((ROOT / "port" / "ps5" / "sce_sys" / "param.json").read_text(encoding="utf-8"))
    param["localizedParameters"]["en-US"]["titleName"] = f"OpenCE {probe.upper()} probe"
    param_path = build / "param.json"
    param_path.write_text(json.dumps(param, indent=2) + "\n", encoding="utf-8")
    icon = ps5_title.plain_icon(build / "icon0.png", (0x2b, 0x3a, 0x5f))
    folder = toolchain.stage(module, build / param["titleId"], param_path, icon, [])
    print(folder)
    return 0


def build_s78() -> int:
    """S7 and S8: sockets and /data/opence (port/ps5/probe/s78.c)"""
    toolchain = ps5_title.Toolchain()
    build = BUILD / "s78"
    flags = ["-std=gnu11", "-O2", "-g", "-fno-omit-frame-pointer", "-ffunction-sections", "-fdata-sections",
             *HOST_WARNINGS]
    objects = [toolchain.compile(PROBE / "s78.c", build / "obj" / "s78.c.o", flags)]
    module = toolchain.link(objects, build)
    param = json.loads((ROOT / "port" / "ps5" / "sce_sys" / "param.json").read_text(encoding="utf-8"))
    param["localizedParameters"]["en-US"]["titleName"] = "OpenCE S7/S8 probe"
    param_path = build / "param.json"
    param_path.write_text(json.dumps(param, indent=2) + "\n", encoding="utf-8")
    icon = ps5_title.plain_icon(build / "icon0.png", (0x5f, 0x3a, 0x2b))
    folder = toolchain.stage(module, build / param["titleId"], param_path, icon, [])
    print(folder)
    return 0


def s7_peer() -> int:
    """S7's other end (port/ps5/probe/s78.c): datagrams to the console's
    port 2302 every half second, each answer confirmed to its sender; and
    TCP connections to the same port, each echoed line confirmed. After a
    confirmation it waits CONFIRMED_PAUSE before trying again, for the next
    run."""
    console = os.environ.get("PS5_HOST", "10.0.1.41")
    pause = 30.0

    def tcp_loop() -> None:
        while True:
            try:
                with socket.create_connection((console, GAME_PORT), timeout=1.0) as connection:
                    connection.settimeout(5.0)
                    line = b"opence-s7 tcp\n"
                    connection.sendall(line)
                    echo = connection.recv(64)
                    print(f"TCP {console}:{GAME_PORT}: echo {echo!r}", flush=True)
                    if echo == line:
                        connection.sendall(b"confirmed\n")
                        time.sleep(pause)
                        continue
            except OSError:
                pass
            time.sleep(0.5)

    threading.Thread(target=tcp_loop, daemon=True).start()
    udp = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    udp.settimeout(0.5)
    print(f"s7-peer: datagrams and TCP connections to {console}:{GAME_PORT}", flush=True)
    quiet_until = 0.0
    try:
        while True:
            if time.monotonic() >= quiet_until:
                udp.sendto(b"opence-s7 hello", (console, GAME_PORT))
            try:
                data, sender = udp.recvfrom(512)
            except socket.timeout:
                continue
            print(f"UDP {sender[0]}:{sender[1]}: {data!r}", flush=True)
            if data.startswith(b"opence-s7 pong"):
                udp.sendto(b"opence-s7 confirmed", sender)
                quiet_until = time.monotonic() + pause
    except KeyboardInterrupt:
        return 0


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("target", choices=("linux", "ps5", "s5a", "s5b", "s6", "s78", "s7-peer"))
    parser.add_argument("--run", action="store_true", help="run the Linux build")
    parser.add_argument("--debug-mesa", action="store_true", help="s5a, s5b, s6: link Mesa's debug build")
    arguments = parser.parse_args()
    if arguments.target == "linux":
        return build_linux(arguments.run)
    if arguments.target in ("s5a", "s5b", "s6"):
        return build_s5(arguments.target, arguments.debug_mesa)
    if arguments.target == "s78":
        return build_s78()
    if arguments.target == "s7-peer":
        return s7_peer()
    return build_ps5()


if __name__ == "__main__":
    sys.exit(main())
