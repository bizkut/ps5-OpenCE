"""Stage the PS5 title's third-party licence texts and source notices."""

import re
import shutil
from pathlib import Path

# These are the licence files shipped by the exact source trees used to build
# the title; the generated dependency trees are pinned by the build scripts.
LICENSES = {
    "OpenCE-AGPL.txt": "LICENSE",
    "SavageWind-and-SDK-GPL.txt": "build/ps5/deps/src/payload_sdk/LICENSE",
    "LLVM-runtime.txt": "port/ps5/LLVM-LICENSE.txt",
    "Mesa-MIT.txt": "build/ps5/mesa/src/licenses/MIT",
    "Mesa-Apache-2.0.txt": "build/ps5/mesa/src/licenses/Apache-2.0",
    "Mesa-SGI-B-2.0.txt": "build/ps5/mesa/src/licenses/SGI-B-2.0",
    "mbedtls.txt": "port/third_party/mbedtls/LICENSE",
    "musl.txt": "build/android/third_party/musl-1.2.5/COPYRIGHT",
    "musl-math.txt": "port/third_party/musl-math/COPYRIGHT",
    "miniupnpc.txt": "port/third_party/miniupnpc/LICENSE",
    "tomlc17.txt": "port/third_party/tomlc17/LICENSE",
    "expat.txt": "port/third_party/expat/COPYING",
    "kcp.txt": "port/third_party/kcp/LICENSE",
    "opus.txt": "port/third_party/opus/COPYING",
    "monocypher.txt": "port/third_party/monocypher/LICENCE.md",
    "zlib.txt": "port/third_party/zlib/LICENSE",
    "Mozilla-CA-MPL-2.0.txt": "port/third_party/cacert/LICENSE",
    "stb-vorbis.txt": "port/third_party/stb_vorbis/LICENSE",
    "stb.txt": "port/third_party/stb/LICENSE",
}

NOTICE = """OpenCE PS5 third-party notices

OpenCE is AGPL-3.0-or-later. Corresponding source: https://github.com/bizkut/ps5-OpenCE
Dependency pins and PS5 Mesa modifications are in port/ps5/deps.lock and
port/ps5/patches/mesa in that source.

Mesa (RADV, Zink and EGL) contains code by many copyright holders; see
Mesa-source-notices.txt and the Mesa licence texts. Mbed TLS is Apache-2.0;
musl and tomlc17 are MIT; miniupnpc is BSD; expat, kcp, opus, monocypher,
zlib, stb and stb_vorbis have their respective texts in this directory.
The Mozilla CA certificate bundle is MPL-2.0.

PS5 input and AudioOut declarations are derived from SavageWindRecomp,
Copyright the Savage Wind Recomp contributors, GPL-3.0-or-later. The PS5
Payload SDK platform layer includes code Copyright 2026 Mihawk under
GPL-3.0-or-later; component notices are in SDK-source-notices.txt. LLVM C++
runtime libraries and compiler builtins are linked; see LLVM-runtime.txt.
"""


def source_notices(root: Path, output: Path) -> None:
    # Include all source copyright/licence blocks, not just those in objects
    # selected by the linker, to include the notices in shared headers too.
    blocks = []
    for path in sorted(root.rglob("*")):
        if path.suffix not in {".c", ".cc", ".cpp", ".h", ".hpp"} or not path.is_file():
            continue
        text = path.read_text(encoding="utf-8", errors="replace")
        comments = re.findall(r"/\*.*?\*/", text, flags=re.DOTALL)
        comments = [c for c in comments if re.search(r"copyright|SPDX-License-Identifier", c, re.IGNORECASE)]
        if comments:
            blocks.append(f"--- {path.relative_to(root)} ---\n" + "\n".join(comments))
    output.write_text("\n\n".join(blocks) + "\n", encoding="utf-8")


def stage(folder: Path) -> None:
    folder.mkdir()
    (folder / "NOTICE.txt").write_text(NOTICE, encoding="utf-8")
    for name, source in LICENSES.items():
        shutil.copyfile(source, folder / name)
    source_notices(Path("build/ps5/mesa/src"), folder / "Mesa-source-notices.txt")
    source_notices(Path("build/ps5/deps/src/payload_sdk/platform"), folder / "SDK-source-notices.txt")
