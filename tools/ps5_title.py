#!/usr/bin/env python3
"""How a PS5 title of this repository is compiled, linked, signed and laid
out as a title folder, with the Forever stack's tools (tools/ps5_deps.py):

- compiled with PS5_Vulkan's prospero-clang18 wrapper over the payload SDK;
- linked with the SDK's prospero-lld, its libc++, libc++abi, libunwind and
  platform layer (libps5platform.a), and clang's builtins: the recipe of
  PS5_Vulkan's tools/radv-link.sh (GPL-3.0-or-later, at the commit
  port/ps5/deps.lock pins), whose comments give the reason for each line,
  with the bindings SavageWindRecomp's ps5/tools/link-recipe.sh added on the
  same SDK after its console runs (getcwd, which faults in a title, and
  isatty, getnameinfo and gai_strerror, null imports in a title);
- converted to an SCE module and signed as eboot.bin by PS5_Vulkan's
  native tool, with the module and companion SDK versions and the fake SELF
  magic its tools/build.sh uses;
- staged as <folder>/eboot.bin, sce_module/libc.prx, sce_sys/param.json and
  sce_sys/icon0.png.
"""

import os
import shutil
import struct
import subprocess
import zlib
from pathlib import Path
from typing import Dict, List, Optional, Sequence, Tuple

import ps5_deps

MODULE_SDK = "0x02000009"
COMPANION_SDK = "0x08050001"
FSELF_MAGIC = "0x1D3D154F"

# libc names the platform layer stands in for (tools/radv-link.sh: functions
# no system module gives a title, or gives it a null import)
PLATFORM_LIBC = (
    "qsort_r mkstemps openlog popen pclose open_memstream __xuname __assert __memset_chk regcomp regexec "
    "regfree regerror localtime_r newlocale freelocale strtod_l strtof_l dladdr utimensat localeconv_l "
    "strtoll_l strtoull_l strtold_l snprintf_l sscanf_l asprintf_l strcoll_l strxfrm_l strftime_l wcscoll_l "
    "wcsxfrm_l btowc_l wctob_l iswctype_l mbrlen_l mbrtowc_l mbsrtowcs_l mbsnrtowcs_l wcrtomb_l wcsnrtombs_l "
    "mbtowc_l ___mb_cur_max_l ___runetype_l ___tolower_l ___toupper_l __runes_for_locale catopen catgets "
    "catclose backtrace backtrace_symbols_fd __cxa_thread_atexit_impl arc4random arc4random_buf "
    "arc4random_uniform gmtime_r statvfs fstatvfs futimens clock_nanosleep getaddrinfo freeaddrinfo "
    "if_nameindex if_freenameindex opendir fdopendir readdir rewinddir dirfd closedir nl_langinfo "
    "nl_langinfo_l getpwuid_r posix_fallocate access openat unlinkat fchmodat fstatat mkdirat renameat "
    "memfd_create getcwd"
).split()
# bound only when the pinned SDK's platform layer defines them
PLATFORM_LIBC_IF_PRESENT = ("localeconv", "strcasestr", "mkstemp", "readlink", "link", "symlink", "isatty",
                            "getnameinfo", "gai_strerror")
# the platform's heap in direct memory, its thread stacks and memstream
PLATFORM_WRAPS = (
    "pthread_create pthread_join pthread_detach fclose fflush malloc calloc realloc free posix_memalign "
    "aligned_alloc memalign malloc_usable_size reallocf reallocarray getline getdelim"
).split()
# kernel imports name a title's libkernel; these stubs give a title nothing
EXCLUDED_STUBS = {"libkernel.so", "libkernel_sys.so", "libkernel_web.so", "libkernel_stub_weak.so",
                  "libc_stub_weak.so"}


class Toolchain:
    """the installed SDK and title tooling"""

    def __init__(self) -> None:
        self.sdk = ps5_deps.payload_sdk()
        self.tooling = ps5_deps.title_tooling()
        self.native_tool = ps5_deps.native_tool()
        self.libc_prx = ps5_deps.libc_prx()
        self.clang = shutil.which("clang") or shutil.which("clang-18")
        if not self.clang:
            raise SystemExit("clang was not found")

    def environment(self) -> Dict[str, str]:
        return {**os.environ, "PS5_PAYLOAD_SDK": str(self.sdk), "PS5_CLANG": self.clang}

    def compile(self, source: Path, output: Path, flags: Sequence[str]) -> Path:
        output.parent.mkdir(parents=True, exist_ok=True)
        subprocess.run(["sh", str(self.tooling / "prospero-clang18"), *flags, "-c", str(source), "-o", str(output)],
                       check=True, env=self.environment())
        return output

    def link_recipe(self, build: Path) -> Tuple[List[str], List[str], List[str]]:
        """(inputs, flags, stubs) as tools/radv-link.sh sets them, without RADV"""
        lib = self.sdk / "target" / "lib"
        platform = lib / "libps5platform.a"
        resource = subprocess.run([self.clang, "--print-resource-dir"], check=True, stdout=subprocess.PIPE,
                                  text=True).stdout.strip()
        builtins = Path(resource) / "lib" / "linux" / "libclang_rt.builtins-x86_64.a"
        inputs = ["-L", str(lib), "--start-group", str(lib / "libc++.a"), str(lib / "libc++abi.a"),
                  str(lib / "libunwind.a"), str(builtins), str(platform), "--end-group"]
        stubs = [str(lib / "libkernel.so"), str(lib / "libkernel_sys.so")]
        stubs += [str(stub) for stub in sorted(lib.glob("*.so")) if stub.name not in EXCLUDED_STUBS]
        flags = [f"--wrap={name}" for name in PLATFORM_WRAPS]
        bound = list(PLATFORM_LIBC)
        symbols = subprocess.run([str(self.sdk / "bin" / "llvm-nm"), "--defined-only", str(platform)],
                                 stdout=subprocess.PIPE, text=True).stdout.splitlines()
        defined = {line.split()[-1] for line in symbols if " T " in line}
        bound += [name for name in PLATFORM_LIBC_IF_PRESENT if f"ps5_{name}" in defined]
        flags += [f"--defsym={name}=ps5_{name}" for name in bound]
        # a bound name the stubs also define would be exported, which the
        # title converter refuses: every one stays local
        version_script = build / "platform-local.map"
        version_script.parent.mkdir(parents=True, exist_ok=True)
        version_script.write_text("{\n    local:\n" + "".join(f"        {name};\n" for name in bound) + "};\n",
                                  encoding="utf-8")
        flags += ["--undefined=_Unwind_Backtrace", "--version-script", str(version_script)]
        for path in [platform, lib / "libc++.a", lib / "libc++abi.a", lib / "libunwind.a", builtins, *map(Path, stubs)]:
            if not path.is_file():
                raise SystemExit(f"missing {path}")
        return inputs, flags, stubs

    def agc_stubs(self, build: Path) -> List[Path]:
        """link stubs for libSceAgc and libSceAgcDriver, system modules the
        SDK has none for, with the functions RADV calls: those
        PS5_Vulkan's tools/build-radv-title.sh links (vendor/ps5/sdk/stubs).
        They only record the imports."""
        stubs = []
        for library, source in (("libSceAgc", "agc_canary_link_stub.c"),
                                ("libSceAgcDriver", "agc_driver_canary_link_stub.c")):
            source_path = self.tooling.parent / "vendor" / "ps5" / "sdk" / "stubs" / source
            obj = self.compile(source_path, build / "stubs" / f"{library}.o",
                               ["-std=c11", "-O2", "-fPIC", "-ffunction-sections", "-fdata-sections"])
            stub = build / "stubs" / f"{library}.so"
            subprocess.run([str(self.sdk / "bin" / "prospero-lld"), "--shared", "-soname", f"{library}.prx",
                            "-o", str(stub), str(obj)], check=True)
            stubs.append(stub)
        return stubs

    def link(self, objects: Sequence[Path], build: Path, radv_archive: Optional[Path] = None,
             archives: Sequence[Path] = ()) -> Path:
        """build/eboot.elf, the SCE module of the objects; with radv_archive,
        RADV linked whole (tools/radv-link.sh: Mesa's dispatch tables name
        entry points through weak references, which pull no archive member in)
        and the other archives (EGL, libgallium) after it"""
        native = self.tooling / "native"
        crt = [self.compile(native / name, build / "obj" / (Path(name).stem + ".o"),
                            ["-std=c++20", "-O2", "-fno-exceptions", "-fno-rtti", "-ffunction-sections",
                             "-fdata-sections"])
               for name in ("app_crt.cpp", "app_cpp_runtime.cpp")]
        inputs, flags, stubs = self.link_recipe(build)
        mesa: List[str] = []
        extra_stubs: List[Path] = []
        if radv_archive:
            mesa = ["--whole-archive", str(radv_archive), "--no-whole-archive"]
            # Mesa's weak entry points stay unresolved-weak rather than dynamic
            flags += ["--no-dynamic-linker", "-z", "nodynamic-undefined-weak"]
            extra_stubs = self.agc_stubs(build)
        mesa += [str(archive) for archive in archives]
        elf = build / "llvm-pie.elf"
        subprocess.run([str(self.sdk / "bin" / "prospero-lld"), "-T", str(self.tooling / "psbc" / "ps5-pie-unwind.ld"),
                        "-L", str(native), "--eh-frame-hdr", "--Map", str(build / "title.map"),
                        "--version-script", str(native / "app-symbols.map"), "--exclude-libs=ALL", *flags,
                        "-e", "_start", "-o", str(elf), *map(str, crt), *map(str, objects), *mesa, *inputs,
                        *map(str, extra_stubs), "--as-needed", *stubs], check=True)
        module = build / "eboot.elf"
        subprocess.run([str(self.native_tool), "link", "--in", str(elf), "--out", str(module),
                        "--stub-dir", str(self.sdk / "target" / "lib"),
                        *(argument for stub in extra_stubs for argument in ("--stub", str(stub))),
                        "--module-sdk", MODULE_SDK, "--companion-sdk", COMPANION_SDK, "--file-name", "eboot.elf"],
                       check=True)
        return module

    def stage(self, module: Path, folder: Path, param: Path, icon: Path, files: Sequence[Path]) -> Path:
        """the title folder: eboot.bin, sce_module/libc.prx, sce_sys/, and
        `files` beside eboot.bin"""
        if folder.exists():
            shutil.rmtree(folder)
        (folder / "sce_sys").mkdir(parents=True)
        (folder / "sce_module").mkdir()
        subprocess.run([str(self.native_tool), "self", "--sign", "--in", str(module), "--out",
                        str(folder / "eboot.bin"), "--magic", FSELF_MAGIC], check=True)
        subprocess.run([str(self.native_tool), "self", "--inspect", "--file", str(folder / "eboot.bin")], check=True,
                       stdout=subprocess.DEVNULL)
        shutil.copyfile(param, folder / "sce_sys" / "param.json")
        shutil.copyfile(icon, folder / "sce_sys" / "icon0.png")
        shutil.copyfile(self.libc_prx, folder / "sce_module" / "libc.prx")
        for path in files:
            shutil.copyfile(path, folder / path.name)
        return folder


def plain_icon(path: Path, rgb: Tuple[int, int, int]) -> Path:
    """a 512x512 PNG of one colour: the launcher's icon for a probe title"""
    width = height = 512
    row = b"\x00" + bytes(rgb) * width
    raw = row * height

    def chunk(kind: bytes, data: bytes) -> bytes:
        return struct.pack(">I", len(data)) + kind + data + struct.pack(">I", zlib.crc32(kind + data))

    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 2, 0, 0, 0))
                     + chunk(b"IDAT", zlib.compress(raw, 9)) + chunk(b"IEND", b""))
    return path
