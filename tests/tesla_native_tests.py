#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Run actual native Tesla method bodies without Horizon or a desktop.

The desktop preview replaces Tesla's renderer/list implementation, so it cannot
exercise these regressions. Extract the production methods unchanged, provide
only platform/render-target stubs, and use the real stb font rasterizer.
"""
import argparse
import os
from pathlib import Path
import shlex
import subprocess
import tempfile


def block(source, signature):
    start = source.index(signature)
    opening = source.index("{", start)
    depth = 1
    end = opening + 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[start:end]


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--cxx", default="c++")
    parser.add_argument("--cxx-flags", default="")
    parser.add_argument("--source", type=Path, default=Path(__file__).resolve().parents[1])
    parser.add_argument("--header", type=Path)
    parser.add_argument("--sanitize", action="store_true")
    args = parser.parse_args()
    root = args.source.resolve()
    header = args.header or root / "overlay/vendor/libtesla/tesla.hpp"
    source = header.read_text()
    render = block(source, "std::pair<u32, u32> drawString(")
    limit = block(source, "std::string limitStringLength(")
    fonts = block(source, "Result initFonts()")
    if "fontForCodepoint(" not in source and not args.header:
        raise ValueError("Native Tesla font selection method is missing")
    # --header allows historical before-fix evidence. Current-source CI must
    # always extract the actual helper above instead of silently substituting it.
    select = block(source, "stbtt_fontinfo* fontForCodepoint(") if "fontForCodepoint(" in source else """
        stbtt_fontinfo* fontForCodepoint(u32 character) {
            if (stbtt_FindGlyphIndex(&m_extFont, character)) return &m_extFont;
            if (m_hasLocalFont && !stbtt_FindGlyphIndex(&m_stdFont, character)) return &m_localFont;
            return &m_stdFont;
        }"""
    item = block(source, "class ListItem : public Element") + ";"
    list_source = source[source.index("class List : public Element"):]
    focus = block(list_source, "virtual Element* requestFocus(")
    template = (root / "tests/tesla_native_harness.cpp.in").read_text()
    for key, value in dict(RENDER=render, LIMIT=limit, FONTS=fonts,
                           SELECT=select, ITEM=item, FOCUS=focus).items():
        template = template.replace(f"@{key}@", value)
    with tempfile.TemporaryDirectory(prefix="cloudtune-tesla-") as directory:
        path = Path(directory)
        generated = path / "native.cpp"
        generated.write_text(template)
        executable = path / "native-tests"
        command = [args.cxx, "-std=c++20", "-O1", "-g", "-I", str(root),
                   "-I", str(root / "overlay/preview"), str(generated), "-o", str(executable)]
        command += shlex.split(args.cxx_flags)
        if args.sanitize:
            command += ["-fsanitize=address,undefined", "-fno-sanitize-recover=undefined",
                        "-fno-omit-frame-pointer", "-fno-pie", "-no-pie"]
        subprocess.run(command, check=True, timeout=45)
        environment = dict(os.environ, UBSAN_OPTIONS="halt_on_error=1:print_stacktrace=1",
                           ASAN_OPTIONS="detect_leaks=1:halt_on_error=1")
        subprocess.run([str(executable), str(root)], check=True, env=environment, timeout=30)


if __name__ == "__main__":
    main()
