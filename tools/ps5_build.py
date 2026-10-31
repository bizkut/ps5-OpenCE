"""Ninja graph for the x32 guest shared by the Linux and PS5 hosts."""

import json
import shutil
from pathlib import Path

from .android_build import LINUX_DIR, SDL_DIR, fetch_third_party, generate_guest_build
from .ps5_host_build import generate_ps5_linux_host
from .ps5_host_build import ANDROID, LINUX, MBED, TOML
from .linux_build import MINIUPNPC_DEFINES, MINIUPNPC_DIR, miniupnpc_sources
from .ps5_licenses import LICENSES

BUILD = Path("build/ps5")
# the title's icon (tools/ps5_title.py's TITLE_ICON)
TITLE_ICON = Path("port/ps5/sce_sys/icon0.png")
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
        abi_flags=GUEST_ABI_FLAGS, musl_extra_flags=("-fno-omit-frame-pointer",))
    host = generate_ps5_linux_host(n, sln, image, host_table_c)
    n.build("ps5_guest", "phony", image)
    n.build("ps5_linux_host", "phony", [image, host])
    if (BUILD / "deps/ps5-payload-sdk/.ps5-sdk-revision").is_file():
        title = generate_ps5_title(n, image, host_table_c)
        n.build("ps5", "phony", title)
    else:
        n.comment("PS5 title build: install the SDK with tools/ps5_deps.py")
    n.newline()


def generate_ps5_title(n, image, host_table_c):
    build = BUILD / "title"
    folder = build / "PPSA99117"
    n.rule("ps5_title_cc",
           command="$python tools/ps5_title.py compile $in $out $cflags -MMD -MF $out.d",
           description="PS5 TITLE CC $out", depfile="$out.d", deps="gcc")
    n.rule("ps5_title_link", command="$python tools/ps5_title.py link --mesa build/ps5/title $in",
           description="PS5 TITLE LINK")
    n.rule("ps5_title_stage",
           command="$python tools/ps5_title.py stage build/ps5/title build/ps5/title/PPSA99117 port/ps5/sce_sys/param.json",
           description="PS5 TITLE SIGN AND STAGE")
    n.rule("ps5_title_identity", command="$python tools/ps5_title.py identity $in $out",
           description="PS5 TITLE IDENTITY")
    identity = build / "identity.h"
    n.build(identity, "ps5_title_identity", image, implicit=["tools/ps5_title.py"])
    flags = (f"-std=gnu11 -O2 -g -ffp-contract=off -fno-omit-frame-pointer -ffunction-sections "
             f"-fdata-sections -D_GNU_SOURCE -DHALO_X32_GUEST=1 "
             f"-I{ANDROID}/include -I{ANDROID}/host -I{ANDROID}/guest/runtime "
             f"-Iport/ps5/host -I{LINUX} -I{TOML} -I{SDL_DIR}/include -I{BUILD}/deps/mesa/include -idirafter port/linux/include")
    sources = sorted(Path("port/ps5/host").glob("*.c"))
    sources += [ANDROID / "host" / (name + ".c") for name in
                ("host_loader", "host_thread", "host_watch_hash", "host_https", "host_gl")]
    sources += [host_table_c, LINUX / "posix_files.c", LINUX / "posix_net.c", TOML / "tomlc17.c"]
    mini = [LINUX / "posix_upnp.c", *miniupnpc_sources()]
    tls = [LINUX / "posix_dtls.c", LINUX / "posix_update.c", *sorted((MBED / "library").glob("*.c"))]
    objects = []
    for source in sources + mini + tls:
        obj = build / "obj" / (str(source).replace("/", "_") + ".o")
        extra = ""
        if source in mini:
            extra = (f" -I{MINIUPNPC_DIR / 'include'} -I{MINIUPNPC_DIR / 'src'} "
                     + " ".join(MINIUPNPC_DEFINES)
                     + " -Dgetaddrinfo=ps5_upnp_getaddrinfo -Dfreeaddrinfo=ps5_upnp_freeaddrinfo")
        if source in tls:
            extra = (f" -I{MBED / 'include'} -I{MBED / 'library'} -DMBEDTLS_THREADING_C "
                     "-DMBEDTLS_THREADING_PTHREAD -DMBEDTLS_NO_PLATFORM_ENTROPY -DMBEDTLS_ENTROPY_HARDWARE_ALT")
        dependencies = ["tools/ps5_title.py"]
        if source == Path("port/ps5/host/main.c"):
            dependencies.append(identity)
            extra += f" -include {identity}"
        n.build(obj, "ps5_title_cc", source, implicit=dependencies, variables={"cflags": flags + extra})
        objects.append(obj)
    image_object = build / "obj/guest_image.o"
    n.build(image_object, "ps5_title_cc", "port/ps5/host/guest_image.S",
            implicit=[image, "tools/ps5_title.py"], variables={"cflags": f"-Wa,-I{image.parent}"})
    objects.append(image_object)
    module = build / "eboot.elf"
    mesa_lib = BUILD / "deps/mesa/lib"
    archives = [mesa_lib / name for name in ("libvulkan_radeon.a", "libgallium.a", "libEGL.a")]
    n.build(module, "ps5_title_link", objects,
            implicit=["tools/ps5_title.py", "tools/ps5_deps.py", "port/ps5/stubs/audioin.c", *archives])
    outputs = [folder / "eboot.bin", folder / "sce_module/libc.prx",
               folder / "sce_sys/param.json", folder / "sce_sys/icon0.png",
               folder / "certs/cacert.pem", folder / "LICENSES/NOTICE.txt",
               folder / "LICENSES/Mesa-source-notices.txt", folder / "LICENSES/SDK-source-notices.txt",
               *[folder / "LICENSES" / name for name in LICENSES]]
    n.build(outputs, "ps5_title_stage", module,
            implicit=["tools/ps5_title.py", "tools/ps5_licenses.py", "port/ps5/sce_sys/param.json",
                      TITLE_ICON, "port/third_party/cacert/cacert.pem", *LICENSES.values(),
                      "build/ps5/mesa/src/.opence-source", "build/ps5/deps/src/payload_sdk/.opence-commit"])
    return outputs
