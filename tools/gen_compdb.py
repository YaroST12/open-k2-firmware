#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Yaraslau Furman
"""Write build/compile_commands.json for src/*.cpp (firmware flags, for clang-tidy / cppcheck).

The ARM toolchain lives in Docker, so the database names the host clang with the firmware's
include paths and defines; tools/static_check.sh adds the ARM target when it runs clang-tidy.
"""
import json
import pathlib

root = pathlib.Path(__file__).resolve().parent.parent
flags = [
    "-std=c++23", "-fno-exceptions", "-fno-rtti", "-Iinclude",
    "-isystem", "third_party/tinyusb/src", "-DCFG_TUSB_MCU=OPT_MCU_AT32F403A_407",
    "-Ithird_party/lwip/src/include", "-Ithird_party/tinyusb/lib/networking",
    "-Ithird_party/littlefs", "-Ibuild/gen", "-DK2_FW_VERSION=\"1.0\"", "-DK2_GIT_HASH=\"x\"",
]
db = []
for f in sorted((root / "src").rglob("*.cpp")):
    rel = f.relative_to(root).as_posix()
    db.append({"directory": str(root), "file": rel,
               "arguments": ["clang++", *flags, "-c", rel, "-o", "/dev/null"]})
out = root / "build" / "compile_commands.json"
out.parent.mkdir(exist_ok=True)
out.write_text(json.dumps(db, indent=1))
print(f"{out}: {len(db)} entries")
