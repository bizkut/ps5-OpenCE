# x32 guest musl architecture

The x32 atomic, relocation, float, fenv and setjmp definitions come from
musl libc 1.2.5, https://musl.libc.org/releases/musl-1.2.5.tar.gz
(SHA-256 a9a118bbe84d8764da0ea0d28b3ab3fae8477fc7e4085d90102b8596fc7c75e4),
licensed under MIT; see the distribution's COPYRIGHT file. The x32 overrides
are atomic_arch.h, pthread_arch.h, reloc.h, and bits/alltypes.h.in, fenv.h,
float.h and setjmp.h. All other architecture files come from the arm64_32
guest directory, which follows x32 in the include path. Both guests share the
kernel interface translated by the host. The thread pointer is obtained from
the guest runtime rather than %fs.
