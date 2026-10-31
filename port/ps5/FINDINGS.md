# PS5 port: findings

Measured facts for docs/ps5_port_plan.md, newest last. Each says where it
ran: on this repository's Linux machine, or on a console (with the run's
number under `build/ps5/evidence/`). A Linux result proves the probe and the
x32 mechanism, never the console.

## 2026-10-31: the x32 guest's image must sit below 2 GB (Linux, clang/lld 18)

The plan placed the x32 image at `0x88000000`, where Android's is. Linking
the probe guest there failed in ld.lld: the image's debug sections' 32-bit
relocations were out of range, and an indexed access to a global
(`int table[64]; table[i]`) compiles under `--target=x86_64-linux-gnux32` to
`movl (,%edi,4), %eax` with the `addr32` prefix and an `R_X86_64_32S`
relocation, a sign-extended displacement, which the small code model allows
only below 2 GB. Linked at `0x7c000000`, right below the Xbox memory, the
image links with its debug information. The plan's address map (D1, D4)
now has the image at `[0x7c000000, 0x80000000)`; only the window's address
is fixed by the game's data.

## 2026-10-31: M0 probe S1-S4 under Linux (x86-64 host, x32 guest)

`python tools/ps5_probe.py linux --run` (S3 shortened to 2 s), on an Intel
Xeon Silver 4116, Ubuntu 24.04, clang 18.1.3: `done: 0 failed check(s)`.

- S1: `[0x7c000000, 0x88000000)` reserved and backed, every page written and
  read back; pools of 1728 MiB at `0x10000000` and 1920 MiB at
  `0x88000000`.
- S2: the loader maps the ELF32 image, fills its six import slots and makes
  its code executable. x32 code called the host with an `int`, a pointer, a
  `float` and a `long long` and got the expected sum; the host read a
  pointer passed through a zero-extending stub. The x32 caller left the
  upper half of a pointer argument's register zero. A guest local lay on
  the guest stack below 4 GB. `probe_work` gave `0x5cafa24ce5720d76` in
  the x32 guest and in x86-64 host code. A guest-to-host call through a stub
  cost 2.87 ns. The guest's MXCSR controls were `0x1f80` (IEEE, no flush to
  zero).
- S3: 8 threads on `pthread_attr_setstack` stacks in the low pool ran x32
  code; every local lay on its own stack, every result matched the host's.
- S4: a write to a read-only page of the window, from the host and from x32
  code, faulted once into the `SIGSEGV` handler, which unprotected the page,
  and the write completed.

The console's own results follow.

## 2026-10-31: run1 on .41: the title's libc.prx sits at the window's base

`build/ps5/evidence/m0/run1.klog`, `run1-opence_probe.txt`; tooling from
`../forever/PS5_Vulkan` `2cf7597`, SDK `fdb2763`.

- Before `main`, the console's loader had mapped the title's
  `sce_module/libc.prx` at `[0x80000000, 0x80118000)`: code (execute-only,
  protection `0x4`) to `0x800cc000`, then read-only and read-write data
  (klog: `/app0/sce_module/libc.prx xotext: 0000000080000000:00000000800cc000`).
  The eboot was at `[0x400000, 0x444000)`; nothing else lay below 4 GB.
  A title cannot leave `libc.prx` out: without it the loader fails before
  the first instruction (PS5_vkQuake `docs/inherited/FINDINGS.md:840-842`).
- `sceKernelReserveVirtualRange` with `MAP_FIXED` over
  `[0x7c000000, 0x88000000)` returned 0 and replaced those mappings without
  a word. The probe's S1 therefore passed when it should not have.
- S2 passed on the console: the x32 image at `0x7c000000`, made executable
  with `sceKernelMprotect` after loading, called the host with each kind of
  argument; `probe_work` gave `0x5cafa24ce5720d76`, as on Linux; a
  guest-to-host call cost 2.01 ns; MXCSR controls `0x1f80`; flexible memory
  442 of 448 MiB free, direct 12,288 MiB.
- S3 crashed 60 s in, at the first `pthread_create`:
  `SYSTEM_XO_VIOLATION` at `0x80000200`, `libc.prx+0x200`, called from
  libkernel's thread start (`0x8000076fb`), on the new thread's stack
  (`rsp 0x101fff08`). Thread creation calls back into `libc.prx`, whose
  code the window had replaced.

Consequence: the window starts above the loader's pages, rounded to the
64 KiB direct-memory unit (`0x80120000` here). The game needs fixed
addresses only from the tag cache up (`0x803A6000`, then the game state at
`0x81A00000`); everything else in the window is handed out top-down
(`port/linux/src/xbox_memory.c:182-199`), and on the Xbox the lowest
physical pages held its kernel. The probe now refuses to reserve a range
anything is mapped in, and S1 fails if the window's free part starts above
`0x803A6000`.

## 2026-10-31: run2 on .41: S1-S4 pass

`build/ps5/evidence/m0/run2.klog`, `run2-opence_probe.txt`; tooling from
`../PS5_Vulkan` `9442a16` (its eboot is byte-identical to `2cf7597`'s for
this probe), SDK `fdb2763`. `done: 0 failed check(s)`; the title ran
62.7 s and left through `sceSystemServiceLoadExec("exit")` with no fatal
signal in the klog.

- S1: `libc.prx` again at `[0x80000000, 0x80118000)`, the same five
  mappings as run1. The image's room `[0x7c000000, 0x80000000)` and the
  window from `0x80120000` to `0x88000000` (126.9 MiB) reserved and backed
  with direct memory, every page written and read back; `libc.prx`'s
  mappings unchanged afterwards. Pools: 1728 MiB at `0x10000000` and
  1920 MiB at `0x88000000`, the first 64 MiB backed.
- S2: as run1 (sum, zero upper half of a pointer argument, guest stack,
  MXCSR controls `0x1f80`, `probe_work` `0x5cafa24ce5720d76`); 2.02 ns per
  guest-to-host call.
- S3: 8 threads on `pthread_attr_setstack` stacks at `0x10100000`
  onwards (1 MiB each) ran x32 code for 60 s, 72,702 to 73,066 rounds of
  `probe_work` each, no mismatch; every result matched the host's.
- S4: a write to a read-only page of the window (`0x80220000`), from the
  host and from x32 code, faulted once into the `SIGSEGV` handler, which
  made the page writable with `sceKernelMprotect`, and the write completed.
  The memory watch's fault method works on the console.

## 2026-10-31: S5a on .41: OpenGL ES 3.2 through Zink on RADV in a plain title

`build/ps5/evidence/m0/run3-s5a` to `run6-s5a` (klog and `opence_s5a.txt`);
`python tools/ps5_probe.py s5a`. Mesa: `../PS5_Mesa` `b260481`
(`feat/videoout-hdr10-vrr`) with `port/ps5/patches/mesa/0001`-`0004`, built by
`tools/ps5_mesa.py` (RADV, Zink and EGL as three archives, the title links
RADV whole); tooling `../PS5_Vulkan` `f410e67`, SDK `fdb2763`. The title
eboot is 33 MB.

- run3: `eglInitialize` failed (`EGL_NOT_INITIALIZED`). With Mesa's debug
  build and `EGL_LOG_LEVEL=debug` (run4): EGL's surfaceless platform looks
  for DRM devices, finds none (no libdrm), and goes on without one only for
  software rendering, where Zink looks for a CPU Vulkan device. Patch 0004
  lets Zink go on without a DRM device when there is no libdrm.
- run5 (debug Mesa) and run6 (release Mesa): `done: 0 failed check(s)`.
  - The guest's addresses (image room, window from `0x80120000`, pools at
    `0x10000000` and `0x88000000`) were reserved before Mesa started, and
    Mesa and RADV ran beside them.
  - RADV's instance: Vulkan 1.4.354, 16 instance extensions, among them
    `VK_KHR_display`, `VK_KHR_surface` and `VK_EXT_headless_surface`.
  - EGL 1.5 initialised in 6.5 ms; the OpenGL ES 3.0 context asked for came
    up as `OpenGL ES 3.2 Mesa 26.2.0`, `GL_RENDERER zink Vulkan
    1.4(PlayStation 5 GPU (RADV NAVI21) (MESA_RADV))`, GLSL ES 3.20, 11.4 ms
    after EGL started.
  - A 3840x2160 gradient drawn by a GLSL ES 3.00 program read back exactly
    (every 97th pixel within 1 of its value; 0 wrong).
  - A second Vulkan device of RADV, beside Zink's, made a `VK_KHR_display`
    swapchain on VideoOut (`PS5 VideoOut`, 3840x2160, first mode 119.880 Hz:
    the title's `attribute3` asks for high frame rate; Mesa logged "119.88 Hz
    kept for title VRR metadata"). 300 frames, each drawn by Zink, read back
    with `glReadPixels` into the second device's host-visible buffer and
    copied to a swapchain image, were presented in 14.71 s (20.4 fps):
    `glReadPixels` of 3840x2160 RGBA8 took 30.6 ms on average (33.7 ms at
    most), which sets that rate. Whether the TV showed the frames was not
    seen; the presents and their fences succeeded.
  - Zink logs `ZINK: vkEnumerateInstanceLayerProperties failed` (RADV
    answers `VK_ERROR_LAYER_NOT_PRESENT` with no loader to list layers) and
    goes on; RADV logs `ac_compute_device_uuid's output is based on invalid
    pci bus info`.

Consequence: S5a passes, and with S1-S3 the M0 gate. The readback path is a
stop-gap at about 20 fps at 4K: S5b (Zink presenting through kopper on
`VK_KHR_display`) is needed for the game's frame rate.

## 2026-10-31: S5b on .41: Zink presents itself on VideoOut at 119.88 Hz

`build/ps5/evidence/m0/run7-s5b` to `run13-s5b`; `python tools/ps5_probe.py
s5b` (`port/ps5/probe/s5_zink.c` with `PROBE_S5B`). Mesa as for S5a, plus
`port/ps5/patches/mesa/0005`: an EGL window surface on the surfaceless
platform is the one display; Zink's kopper makes a `VK_KHR_display` plane
surface on the display mode of the window's size and presents to it.

- run11-run13, one window size each (`S5B_SIZE` in `s5a-env.txt`): all
  `done: 0 failed check(s)`.

  |Window|Frames|Rate|Interval (after frame 60)|
  |---|---|---|---|
  |3840x2160|1200 in 9.99 s|120.15 fps|8.323 ms mean, 8.24-8.44 ms|
  |2560x1440|1200 in 9.99 s|120.14 fps|8.324 ms mean, 8.26-8.43 ms|
  |1920x1080|1200 in 9.99 s|120.12 fps|8.325 ms mean, 8.23-8.45 ms|

  Kopper took the display mode of each size at 119.880 Hz (the title's
  `attribute3` asks for high frame rate), and VideoOut registered a set of
  five buffers of that size. The drawing is one full-screen triangle, so
  these rates are the display's pace, not the cost of the game's frames:
  M6 measures that (D11). The gradient read back exactly in the window's
  back buffer at each size. Making the window current took about 2.1 s
  from the start of EGL (the swapchain and VideoOut's mode), against 11 ms
  for S5a's pbuffer.
- run7-run9 changed the window's size within one run, and showed two
  things. A display has one swapchain at a time: the second window's
  swapchain failed with `VK_ERROR_NATIVE_WINDOW_IN_USE_KHR` while Zink had
  not yet destroyed the first's, which it does after the GPU's work on it,
  not when the EGL surface is destroyed (even after `glFinish`). And until
  0005 keyed display targets by size, kopper reused the first window's
  target for the next, so a 1080p window still drew into the 4K swapchain.
  Consequence: the game makes its window once, at the size it starts with;
  a change of output size applies at the next start.
- With the swapchain missing, `eglSwapBuffers` still returned `EGL_TRUE`
  and frames ran at 20,000 a second (run8, run9); the probe now requires
  the display's pace (a mean interval of 7 ms or more) for a pass.

## 2026-10-31: the game's own resolution (the owner's disc image, Linux)

The owner's disc image, `Halo - Combat Evolved (USA) (Rev 2).xiso.iso`
(3,625,713,664 bytes, in `/data/opence` on .41), copied to `assets/`
(ignored by git) and its `maps` folder read out: 24 maps, 1.8 GB. Nothing
of it is in the repository; only these counts are.

- The Xbox drew 640x480; the HUD and menus are laid out on its 480 lines
  (`port/linux/README.md`: `display.resolution_scaling = "original"`).
- Bitmaps, read with `tools/hud_assets.py`'s Xbox map reader: 3,751 (tag,
  bitmap) pairs across the maps, 3,694 2D, 51 cube maps, 6 3D. By the
  longer side: 32 or less 9.0 %, 64 16.3 %, 128 27.6 %, 256 35.6 %, 512
  9.5 %, 1024 73 bitmaps, 2048 2. The median is 128 in every map but
  `ui.map` (256). The largest are the Halo ring's 2048x128 strip, menu
  backgrounds and banners (1024x512, 1024x256), and some sky and decal
  strips (1024x128).

Consequence: at 3840x2160 a typical wall's 128- or 256-texel texture spans
hundreds of pixels; more resolution sharpens edges, the HUD and text, not
the textures (plan D11).

## 2026-10-31: S8 on .41: `/data/opence`, and the maps read in place (run14-run16)

Probe `port/ps5/probe/s78.c` (`tools/ps5_probe.py s78`), an unelevated
title, ShadowMountPlus loaded on .41.

- `/data/opence` is a directory the title sees (mode 777). 4 MiB written in
  1 MiB writes and synced took 207.9 ms (19.2 MiB/s, the same in all three
  runs), renamed over an existing file, read back identical (mode 666),
  removed with its folder.
- The owner's disc image is read in place: the XDVDFS volume at offset 0
  (an xiso), a root table of 88 bytes and 4 entries, a `maps` folder of 24
  files and 1,860,638,720 bytes, each with a cache file header (`daeh`
  first, `toof` last). `a10.map` (173,903,872 bytes) read out of the image
  on the console has CRC-32 `7b1e98a0`, the same as zlib's of the file
  extracted from the image's copy on Linux.
- Reads, by run:

  |Run|`d40.map`: 256 random 64 KiB reads, mean (slowest)|`a10.map` whole in 1 MiB reads|
  |---|---|---|
  |run14 (first after the image's upload)|1.64 ms (3.11 ms), 38.2 MiB/s|120.0 MiB/s, slowest 12.5 ms|
  |run15|0.74 ms (0.81 ms), 84.5 MiB/s|80.7 MiB/s, slowest 13.3 ms|
  |run16|0.78 ms (1.07 ms), 79.9 MiB/s|80.7 MiB/s, slowest 14.0 ms|

  Savage Wind measured an unelevated title at 133 MiB/s sequential and
  1.5-1.6 ms per random 64 KiB read (its `ps5/FINDINGS.md:109-111`). A
  campaign map of about 170 MB takes 1.5-2.1 s to read.
- Not run: a console without ShadowMountPlus (it is loaded on .41).

Consequence: plan D7 holds. The title reads the maps from the disc image;
nothing of the image needs copying.

## 2026-10-31: S7 on .41: sockets (run14-run16)

The same probe, with `tools/ps5_probe.py s7-peer` on this Linux machine.
The network: this machine (10.1.1.15) reaches the console's LAN
(10.0.1.0/24) through a Tailscale subnet router, so the console sees the
peer's traffic as coming from the router's LAN address, 10.0.1.10; no
machine of this project is on the console's own LAN. In run14 the title
sent first, to the machine's tailnet address 100.107.135.71: no datagram
arrived and TCP timed out (errno 60), because the console's LAN has no
route there. From run15 the peer starts each exchange.

- UDP port 2302 binds (runs 14-16). The peer's datagram arrives on 2302,
  the title's answer from 2302 reaches the peer, and the peer's
  confirmation arrives (run15, run16).
- A TCP socket listens on 2302; the peer connects (0.2 s after the listen),
  its line is echoed and the echo confirmed (run15, run16).
- `SO_BROADCAST` is accepted; a datagram to 255.255.255.255:2302 is sent
  and comes back to the title's own socket from 10.0.1.41 (runs 14-16).
  Whether another machine on the LAN receives it is not measured.
- libc's `getaddrinfo` returns 4 (`EAI_FAIL`) at once: the title link binds
  it to the platform layer's refusal (`ps5_getaddrinfo`, as
  `tools/ps5_title.py` takes from `radv-link.sh`). libSceNet's resolver
  (`sceNetPoolCreate` of 0x4000 bytes, `sceNetResolverCreate`,
  `sceNetResolverStartNtoa`, as Forever's
  `ps5-wow/ps5/probes/psmkit-wine/native-entry.c:1145-1190` calls them)
  resolved github.com in 200.7 ms (run16), and a TCP connection to it on
  port 443 was made in 231.5 ms.
- A DNS query over UDP to 1.1.1.1 was answered in 103.7 ms (run14) and
  110.1 ms (run16). In run15 the probe's single try had no answer within
  3 s; since run16 it tries twice each at 1.1.1.1 and 8.8.8.8, and the
  first try was answered.
- DTLS itself was not run: it needs Mbed TLS (M8).

Consequence: the host's name lookups use libSceNet's resolver (plan D8).

## 2026-10-31: S6 on .41: pad, keyboard, mouse and AudioOut beside Zink (run17)

Probe `port/ps5/probe/s5_zink.c` built with `PROBE_S6` and
`port/ps5/probe/s6_devices.c` (`tools/ps5_probe.py s6`): S5b's window at
3840x2160 for 20 s while the devices are read each frame and a tone plays.
The calls and records are those of SavageWindRecomp's `runtime/ps5/input.c`
and `audio.c`; nobody was at the console.

- Presentation unchanged by the devices: 2400 frames in 20.00 s
  (120.03 fps), intervals 8.26-8.44 ms after frame 60.
- User service 0, initial user 486509363; `scePadInit` 0, `scePadOpen`
  gave a handle; 2400 `scePadRead` calls returned 12,045 records, no error,
  the pad connected (a DualSense on the console; no button or stick moved).
- Keyboard and mouse modules loaded, `sceKeyboardInit` and `sceMouseInit`
  0, indexes 0 and 1 of each opened; 4800 reads of each kind, no error, no
  device connected.
- `sceAudioOutOpen(0xff, main, 0, 256, 48000, S16 stereo)` opened a port:
  3753 periods output in 20.00 s (3750 at 48 kHz), no error, no return
  later than two periods, slowest 5.57 ms.
- No VideoOut or AudioOut error in the klog. (The `PFAuthClient` errors and
  one `process_starter_util.cpp` line are in every klog since run11,
  S5b's included.)

Not done, needing a person at the console: DualSense buttons and sticks, a
USB keyboard's keys, whether the tone is heard.

Consequence: plan D6 implements the `host_sdl_*` imports on these
libraries; pacbrew's SDL3 is not used.

## 2026-10-31: M1, the x32 guest under a Linux host (this machine, no console)

`python3 configure.py --portable && ninja ps5_linux_host` builds
`build/ps5/halo_guest.elf` (ELF32 x86-64 at `0x7c000000`: code
`0x7c000000`+`0x1107843`, data `0x7d110000`+`0x1cb95f8`, the image ending at
`0x7edca000`; no undefined symbol) and `build/ps5/linux/halo-x32`, which
holds the image and loads it with 221 imports, none missing. Clang 18,
Mesa 25.2.8 llvmpipe, Xvfb.

- The main menu draws (OpenGL ES 3.2 through the host's GLES imports;
  `build/ps5/linux/menu.png`). A hosted game on bloodgulch
  (`HALO_NETWORK_TEST=host:bloodgulch`, `HALO_EXIT_AFTER=60`) logged a
  player line each second and quit through `debug.exit_after` with status 0,
  no fault.
- `tools/net_compare.py`, the x32 build against the i386 build
  (`build/linux/halo`, `--portable`), bloodgulch, 60 s compared each way:
  both pass; positions apart mean 0.044 and 0.047, largest 0.098 and 0.101
  world units; drift 0 of 1920 ticks both ways (`build/net_compare/m1-final`).
  An earlier pair of runs had drift in the x32-hosted game (346 of 1608
  ticks, largest 0.022); control runs of two identical builds had as much
  (i386 with i386: 229 of 744 ticks, largest 0.0177; x32 with x32: 336 of
  1203, largest 0.0166), so it is the netcode's, not the build's.
- `tools/stagekit/tests` with `HALO_GAME=build/ps5/linux/halo-x32`: 20
  passed (each machine's `game.log` names the x32 image), as with the i386
  build.
- x87 instructions in the image: 380, all in 17 musl functions
  (`__fesetround`, `__floatscan`, `copysignl`, `fabsl`, `feclearexcept`,
  `fegetenv`, `fesetenv`, `fetestexcept`, `fmodl`, `frexpl`, `hexfloat`,
  `pop_arg`, `printf_core`, `scalbnl`, `strtod`, `strtof`, `vfscanf`): the
  floating-point environment and `long double` conversion; none in the
  game's or the port's code.
- Boundary layouts under arm64_32 and x32 (clang record dumps,
  `build/ps5/layout-*.txt`): equal for `halo_guest_header` (48/4),
  `halo_guest_boot` (16/4), the kernel `kstat` (128/8), `stat` (88/8),
  `timespec`, `iovec`, `pollfd` (8/4), `rlimit` (16/8), `dirent` (280/8),
  `utsname` (390/1), `posix_file_information` (36/4), `SDL_AudioSpec`
  (12/4), `SDL_Event` (128/8) and all 122 SDL records compared.
- Pointer arguments: clang 18's x32 code zero-extends them at the call
  (`addl $8, %edi` for a member's address, `addl %esi, %edi` for pointer
  arithmetic) and a function forwarding its pointer argument compiles to a
  bare `jmp`, relying on its caller; the import stubs therefore pass
  registers through.
- Android, which this machine cannot build (no NDK): every changed
  Android-compiled file preprocesses to the same code with Android's
  defines; Android's generator outputs and its `build.ninja` text (with a
  stand-in NDK) are identical to before.
- Baseline failures on this machine, at the original commit too
  (`da8d4556`, clang 18): `tools/harness` `test_polygon_clip.py`'s two
  `nan-crossing` negative controls and `test_elevator_prediction.py`'s
  `riding-up`.

- `tools/android_abi_check.py` on LLVM IR of the 576 game, platform and
  runtime files the x32 image compiles (their `build.ninja` commands with
  `-S -emit-llvm`): 0 unsafe declarations.
- This machine's kernel cannot run x32 programs (`CONFIG_X86_X32_ABI` is
  not set; a `clang -mx32` executable does not start), so `tools/harness`
  cannot run its engine functions as native x32 programs here; they would
  have to be guest images run by `halo-x32 --image`.

Not done: `tools/harness` against x32 (as guest images), and a
`tools/guest_layout_check.py` in place of the hand comparison.

## 2026-10-31: M2 on .41: the title runs the x32 guest (run1-run22)

`ninja ps5` (`build/ps5/title/PPSA99117`), no Mesa: the game windowless.
`build/ps5/evidence/m2/runN.klog`; from run4 each run's eboot, host ELF and
guest ELF are kept in `runN/`. The env file `/app0/opence-env.txt` sets the
guest's environment; the maps are in `/data/opence-dev/maps` (copied over
FTP: the 13 multiplayer maps and `ui.map`).

- Start: the first klog line names the build and the embedded image
  (`opence: PPSA99117 <describe> image <sha256 prefix>`); the host reserves
  `[0x7c000000, 0x80000000)` and the window `[0x80120000, 0xA0000000)`,
  copies the image to `0x7c000000`, makes its code executable and runs the
  game's start-up (every run from run2).
- Randomness: `libSceRandom.sprx` loaded (status 0) but its function was
  not found by name (run2) nor by NID (`0x80020003`, run3), and a direct
  import of `sceRandomGetRandomNumber`, listed as NEEDED, was still address
  0 when called (run4). `/dev/urandom` gave 32 bytes at once (run5, every
  run since).
- Direct memory is not zero when allocated: musl's mallocng found its guard
  byte (in a mapping's tail beyond the requested size) nonzero and stopped
  (run5, SIGBUS at its `hlt`). Clearing every new 64 KiB unit and each
  mapping's whole tail removed it (run6).
- `debug.exit_after` is checked where SDL's events are pumped, which happens
  only once SDL has started: with `host_sdl_init` failing, the game never
  quit (run6-run8, 300 s, 60 s, 30 s), and the Linux host did the same
  without a display, while it quit after 5.3 s with one. The PS5 host's
  `host_sdl_init` now succeeds and window creation fails, so the game runs
  windowless and quits on time (run9).
- Sockets: on a UDP socket, `fcntl(F_SETFL, O_NONBLOCK)` and
  `ioctl(FIONBIO)` fail with `EACCES`, `fcntl(F_SETFD, FD_CLOEXEC)` with
  `EINVAL`; `setsockopt(SOL_SOCKET, 0x1200 /* SO_NBIO */)` succeeds and a
  `recv` with nothing pending then returns `EAGAIN` at once (run15). With
  that, internet play's socket opens and both STUN servers (Google's,
  Cloudflare's) answer with the console's public address (run16).
- A hosted game's default playlist loads `carousel.map` before the network
  test chooses its map; without it the game shows its damaged-media screen
  (run17-run18, found by sampling the main thread and tracing map opens).
- Acceptance, one eboot (`c0bfa136…`, image `4171b872b7a2`): run19 hosted
  slayer on bloodgulch windowless, a player line every 30 ticks from 30 to
  1230, and left through `debug.exit_after` and
  `sceSystemServiceLoadExec("exit")` after 60.7 s; run20-run22, the
  windowless start-up with `HALO_EXIT_AFTER=5`, each left the same way after
  5.8 s. No fatal signal in any of the four klogs.
- The host's diagnostics, on only when the env file sets
  `OPENCE_HOST_WATCHDOG=<seconds>`: each guest thread's current system call
  and how long it has been in it, futex waits and the wakes their address
  received, samples of the main thread's guest stack (`SIGUSR1`, frame
  pointers; the x32 guest's musl is built with frame pointers for it), and
  the map files' opens and reads.

Checked on this machine after the M2 changes: `ninja linux ps5_linux_host`
build; the Linux host plays a hosted bloodgulch game for 60 s under Xvfb
and exits 0; `tools/stagekit/tests` pass with the x32 build (20); every
Android-compiled file changed preprocesses as before with Android's defines
(with the headers M2 split out inlined), except `xbox_memory.c`, whose new
code runs only when `HALO_WINDOW_FLOOR` is set, which only the PS5 host
does.

## 2026-10-31: M3 first menu frames on .41 (run1-run5)

`build/ps5/evidence/m3/runN` (each with its eboot and ELF). Runs 1-2
repeat M2's windowless acceptance after the PS5 host's style pass (eboot
`e7ee23b8…`): the five-second start-up and the 60-second hosted bloodgulch
game both left through `debug.exit_after` and `LoadExec(0)`.

The title now links Mesa's EGL, Zink and RADV (eboot 56 MB). From run3 the
game draws its main menu at 3840x2160 through EGL's surfaceless platform,
Zink and kopper on VideoOut, mode 3840x2160 at 119.880 Hz with the VRR
metadata: `GL OpenGL ES 3.2 Mesa 26.2.0 renderer zink Vulkan 1.4(PlayStation
5 GPU (RADV NAVI21) (MESA_RADV))`. Every run left through `debug.exit_after`
and `LoadExec(0)`, no fatal signal.

- Start-up (run5): the window in 7.8 ms, the context current 2077.2 ms
  after it (Zink and RADV starting), the first swap at 2102.7 ms.
- Frame intervals at the swap, 10-second windows (run5, no screenshots):
  the first 1104 frames, mean 9.064 ms, longest 783.151 ms; then 1199
  frames each window, mean 8.342 ms, longest 8.456 and 8.428 ms: the
  display's 119.88 Hz with nothing late. The cause of the first window's
  long frame is not measured (shader compiles are likely: the shader cache
  is off). The owner, watching run4 on the TV: "it runs smooth".
- Run4, no screenshots: its first window's longest frame 775 ms, then
  three windows of mean 8.342 ms, longest 8.513, 8.496 and 8.462 ms. Run3
  saved a 4K BMP screenshot periodically; its windows had frames of 4.3
  and 6.4 s, from the captures.
- The picture, run3's capture against the Linux host's at the same size and
  settings (llvmpipe; `ps5.png`, `linux.png`): the same menu. Over the
  whole frame the channels differ by 7.24, 8.27 and 8.98 on average and
  36.61 % of pixels by more than 8, because the background is the ring
  scene, animated by time, caught at another moment. On the parts drawn
  over it: the Halo logo's 86,393 bright pixels differ by 0.45-0.49 on
  average, 0.27 % by more than 8; the menu items' 38,769 bright pixels by
  3.2-3.4, 19.6 % by more than 8 (their glow is blended over the moving
  background, and the highlighted item pulses).

## 2026-10-31: M4 input and audio on .41 (run1-run4)

The run artifacts and klogs are in `build/ps5/evidence/m4/run1` and
`run2`. Both ran the menu with audio enabled for 70 seconds and exited
through `debug.exit_after` and `LoadExec(0)`, without a title fatal signal.
Run1 eboot SHA-256:
`d39f9387891003266bb2eb181e2ce3e4d662f9b68f702c37731548ae02d51b07`;
run2:
`7d8e4116bf06ebb2c6c1e2a3e2f460380d6b7f46d61c55927bb9690570e92a74`.

- Run2 opened a pad for user 486509363; keyboard and mouse module loads and
  initializations returned zero. AudioOut opened and six 10-second windows
  reported 1876-1878 periods, zero underruns and zero errors.
- During run1 the owner at the TV reported hearing sounds. Its klog recorded
  a pad button mask `0x40`; whether that was the owner's action is unknown.
- Run1 logged unrecognized keyboard binding names; run2 included the
  scancode-name mapping and did not log those warnings. Keyboard and mouse
  physical interaction, split screen, multiple users, hotplug and rumble
  have not been qualified by these runs.

Run3 (eboot SHA-256
`dec10d3697a4a89b659c017970ebc392881d30c593991d0d95b5807e2d642031`)
adds window shown and focus-gained events. Before the fix, the owner reported
that the controller did nothing despite host stick logs; after it, the owner
reported the controller working in run3. The klog recorded
`PS5 window 1 focus gained event delivered`, pad button changes, and a
connection to internet host `46b43b607cd0` (lines 1063-1066). At 790 s
it exited through `LoadExec(0)` without the `debug.exit_after` marker; a
menu Quit is possible but the log does not establish the cause.

Run4 (eboot SHA-256
`069640e380eed917b805a386aafdcfa07b513dfdfef9ff2b484b8f3d076e1384`):
the owner reported “Push to talk works” in an internet game joined to host
`46b43b607cd0`. The klog at lines 1158-1162 records touchpad press, AudioIn
opening for the user, touchpad release and a nonzero microphone peak; lines
1272-1279 record another press and release and nonzero input. The owner
also reported “open mic works” and that the DualSense mute button cut their
voice in open-mic mode. The microphone remained open and its klog showed
nonzero input (for example lines 1333-1341), but does not identify the voice
mode or correlate a mute transition with a particular input buffer. The
mute button's exact AudioIn representation therefore remains unmeasured.
Run4 ended after 366 s through `LoadExec(0)` after leaving the network host;
the runner returned 3 because it did not see `debug.exit_after`.

Run5 (eboot SHA-256
`0d690640da6120da522bd10702682c511537915c4a834f87f35cfcff32556ac1`, the
release candidate's eboot: the restyled backends, the users already signed
in taken at start with `sceUserServiceGetLoginUserIdList`): both users
signed in on the console, 486509363 and 486509364, got a pad; the window at
3840x2160, the first frame 2118.4 ms after it; then 1194 and 1192 frames in
10-second windows (8.377 and 8.395 ms mean); AudioOut without underrun or
error; `debug.exit_after` and `LoadExec(0)` after 40 s, no fatal signal.
Split screen itself was not played.

After run5 the owner started the game from the home screen twice and
reported a crash. It was not one: run5's `/app0/opence-env.txt`, still on
the console, set `HALO_EXIT_AFTER=40`, and `/data/opence-dev/opence.log`
shows both sessions ending with `exiting after debug.exit_after` and
`LoadExec exit (0)` after 40 s, the first one in a game's map. The release
build (eboot `230f448f…`, "Halo: Combat Evolved (OpenCE)") is now installed
with an env file that sets only the data and save roots
(`/data/opence-dev`, where the owner's maps, settings and profiles are).
Console runs leave such a file behind; after a test, put back one without
`HALO_EXIT_AFTER`.
