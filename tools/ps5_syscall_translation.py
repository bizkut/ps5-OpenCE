"""Generate the x32 guest to Linux x86-64 host syscall number translation."""

import re
import sys
from pathlib import Path


def main() -> None:
    numbers_path, dispatcher_path, output_path = map(Path, sys.argv[1:])
    guest_numbers = dict(re.findall(
        r"^#define\s+__NR_(\w+)\s+(\d+)$", numbers_path.read_text(), re.M
    ))
    cases = set(re.findall(r"case SYS_(\w+):", dispatcher_path.read_text()))
    lines = ["#include <sys/syscall.h>", "long host_translate_syscall(long long n)",
             "{", " switch (n) {"]
    for name in sorted(cases):
        if name in guest_numbers:
            lines.extend([f"#ifdef SYS_{name}",
                          f" case {guest_numbers[name]}: return SYS_{name};", "#endif"])
    lines.extend([" default: return -1;", " }", "}", ""])
    output_path.parent.mkdir(parents=True, exist_ok=True)
    output_path.write_text("\n".join(lines))


if __name__ == "__main__":
    main()
