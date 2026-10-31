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
