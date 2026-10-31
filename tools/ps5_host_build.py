"""Linux x86-64 host for the PS5 x32 guest."""

import subprocess
from pathlib import Path

from .android_build import SDL_COMMIT, SDL_TAG, SDL_URL
from .linux_build import MINIUPNPC_DEFINES, MINIUPNPC_DIR, miniupnpc_sources

BUILD = Path("build/ps5/linux")
SDL = BUILD / "third_party/SDL3"
SDL_BUILD = BUILD / "sdl3"
ANDROID = Path("port/android")
LINUX = Path("port/linux/src")
MBED = Path("port/third_party/mbedtls")
TOML = Path("port/third_party/tomlc17")


def generate_ps5_linux_host(n, sln, image: Path, host_table_c: Path) -> Path:
    del sln
    if not SDL.exists():
        SDL.parent.mkdir(parents=True, exist_ok=True)
        subprocess.run(["git", "clone", "--depth", "1", "--branch", SDL_TAG, SDL_URL, str(SDL)], check=True)
    revision = subprocess.check_output(["git", "-C", str(SDL), "rev-parse", "HEAD"], text=True).strip()
    if revision != SDL_COMMIT:
        raise RuntimeError(f"SDL3 {SDL_TAG} is {revision}, not {SDL_COMMIT}")

    # Both guests use asm-generic numbers; the Linux host uses x86-64 numbers.
    translation = BUILD / "syscall_translation.c"
    n.rule("ps5_linux_syscall_translation",
           command="$python tools/ps5_syscall_translation.py $in $out",
           description="PS5 LINUX SYSCALL TRANSLATION")
    n.build(translation, "ps5_linux_syscall_translation",
            [ANDROID / "guest/libc/arch/arm64_32/bits/syscall.h.in",
             ANDROID / "host/host_syscall.c"],
            implicit=["tools/ps5_syscall_translation.py"])

    libsdl = SDL_BUILD / "libSDL3.so"
    n.rule("ps5_linux_sdl", command=(f"cmake -S {SDL} -B {SDL_BUILD} -G Ninja -DCMAKE_BUILD_TYPE=Release "
           "-DSDL_SHARED=ON -DSDL_STATIC=OFF -DSDL_TESTS=OFF -DSDL_EXAMPLES=OFF "
           "-DSDL_X11=ON -DSDL_WAYLAND=ON -DSDL_OPENGLES=ON "
           f"&& ninja -j1 -C {SDL_BUILD}"), description="PS5 LINUX SDL3", pool="console")
    n.build(libsdl, "ps5_linux_sdl", implicit=[SDL / "CMakeLists.txt"])
    n.rule("ps5_linux_host_cc", command="clang -MMD -MF $out.d $cflags -c $in -o $out",
           description="PS5 HOST CC $out", depfile="$out.d", deps="gcc")
    flags = (f"-std=gnu11 -O2 -g -D_GNU_SOURCE -DHALO_X32_GUEST=1 -Wall -Wno-unused-function "
             f"-I{ANDROID}/include -I{ANDROID}/host -I{SDL}/include -I{LINUX} -I{TOML}")
    sources = [ANDROID / "host" / name for name in ("host_loader.c", "host_memory.c", "host_thread.c",
               "host_syscall.c", "host_sdl.c", "host_gl.c", "host_debug.c", "host_watch_hash.c", "host_https.c")]
    sources += [Path("port/ps5/host/linux/main.c"), Path("port/ps5/host/linux/syscall.c"), translation, host_table_c,
                LINUX / "posix_files.c", LINUX / "posix_net.c", TOML / "tomlc17.c"]
    mini = [LINUX / "posix_upnp.c", *miniupnpc_sources()]
    tls = [LINUX / "posix_dtls.c", LINUX / "posix_update.c", *sorted((MBED / "library").glob("*.c"))]
    objects = []
    for source in sources + mini + tls:
        obj = BUILD / "obj" / (str(source).replace("/", "_") + ".o")
        extra = ""
        if source in mini:
            extra = f" -I{MINIUPNPC_DIR / 'include'} -I{MINIUPNPC_DIR / 'src'} " + " ".join(MINIUPNPC_DEFINES)
        if source in tls:
            extra = f" -I{MBED / 'include'} -I{MBED / 'library'} -DMBEDTLS_THREADING_C -DMBEDTLS_THREADING_PTHREAD"
        n.build(obj, "ps5_linux_host_cc", source, order_only=[libsdl], variables={"cflags": flags + extra})
        objects.append(obj)
    # the guest image, linked in (port/ps5/host/linux/guest_image.S): the executable then runs on its own
    # wherever it is copied, as the desktop build's does
    image_object = BUILD / "obj" / "guest_image.o"
    n.build(image_object, "ps5_linux_host_cc", Path("port/ps5/host/linux/guest_image.S"), implicit=[image],
            variables={"cflags": f"-Wa,-I{image.parent}"})
    objects.append(image_object)
    # SDL beside the executable, as the desktop build's (build/linux/libSDL3.so.0): tools that copy the game
    # take the executable and the libraries beside it
    staged_sdl = BUILD / "libSDL3.so.0"
    n.rule("ps5_linux_copy", command="cp -L $in $out", description="PS5 LINUX COPY $out")
    n.build(staged_sdl, "ps5_linux_copy", libsdl)
    n.rule("ps5_linux_host_link",
           command=(f"clang -o $out @$out.rsp -L{SDL_BUILD} -Wl,-rpath,'$$ORIGIN' -lSDL3 -lGLESv2 -lEGL -ldl -lm "
                    "-pthread"),
           description="PS5 HOST LINK $out", rspfile="$out.rsp", rspfile_content="$in_newline")
    executable = BUILD / "halo-x32"
    n.build(executable, "ps5_linux_host_link", objects, implicit=[libsdl, staged_sdl])
    return executable
