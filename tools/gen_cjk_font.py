"""Generate a GB2312 + ASCII 16x16 bitmap font subset from GNU Unifont.

Usage:
    python tools/gen_cjk_font.py [unifont.hex]

Outputs:
    main/board/font_cjk.h
    main/board/font_cjk.cpp
    tools/cache/font_preview.png (if Pillow is available)

Unifont is licensed under the SIL Open Font License 1.1 (and GPLv2+ with
font embedding exception); see licenses/unifont-*.txt.
"""
import os
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DEFAULT_HEX = os.path.join(ROOT, "tools", "cache", "unifont.hex")
OUT_H = os.path.join(ROOT, "main", "board", "font_cjk.h")
OUT_CPP = os.path.join(ROOT, "main", "board", "font_cjk.cpp")


def load_unifont(path):
    glyphs = {}
    with open(path, "r", encoding="ascii", errors="ignore") as f:
        for line in f:
            line = line.strip()
            if not line or ":" not in line:
                continue
            cp_s, data = line.split(":", 1)
            try:
                cp = int(cp_s, 16)
            except ValueError:
                continue
            data = data.strip()
            if len(data) not in (32, 64):
                continue
            glyphs[cp] = bytes.fromhex(data)
    return glyphs


def gb2312_codepoints():
    cps = set()
    for hi in range(0xA1, 0xF8):
        for lo in range(0xA1, 0xFF):
            try:
                ch = bytes([hi, lo]).decode("gb2312")
            except UnicodeDecodeError:
                continue
            if len(ch) == 1:
                cps.add(ord(ch))
    return cps


def main():
    hex_path = sys.argv[1] if len(sys.argv) > 1 else DEFAULT_HEX
    raw = load_unifont(hex_path)
    print(f"unifont glyphs: {len(raw)}")

    cps = set(range(0x20, 0x7F)) | gb2312_codepoints()
    # Common punctuation / symbols that may not be in GB2312.
    cps |= {0x00B7, 0x2014, 0x2018, 0x2019, 0x201C, 0x201D, 0x2026, 0x3000, 0x00D7}

    entries = []
    missing = 0
    for cp in sorted(cps):
        data = raw.get(cp)
        if data is None:
            missing += 1
            continue
        if len(data) == 16:
            width = 8
            bitmap = bytearray(32)
            for row in range(16):
                bitmap[row * 2] = data[row]
        else:
            width = 16
            bitmap = bytearray(data)
        entries.append((cp, width, bitmap))

    # Always keep '?' and a space for fallback.
    have = {e[0] for e in entries}
    for cp in (0x20, 0x3F):
        if cp not in have:
            entries.append((cp, 8, bytearray(32)))
    entries.sort(key=lambda e: e[0])

    print(f"subset glyphs: {len(entries)} (missing {missing})")

    os.makedirs(os.path.dirname(OUT_H), exist_ok=True)
    with open(OUT_H, "w", encoding="utf-8") as f:
        f.write("""#pragma once

#include <cstdint>

/* Generated from GNU Unifont 18.0.01 (GB2312 + ASCII subset).
 * Unifont is licensed under SIL OFL 1.1; see licenses/unifont-OFL-1.1.txt. */
struct CjkGlyph {
    uint32_t cp;
    uint8_t width;   // 8 for half-width ASCII, 16 for CJK
    uint8_t bitmap[32];  // 16 rows, 2 bytes per row, MSB first
};

extern const CjkGlyph kCjkGlyphs[];
extern const int kCjkGlyphCount;
""")

    with open(OUT_CPP, "w", encoding="utf-8") as f:
        f.write("""#include "font_cjk.h"

/* Generated from GNU Unifont 18.0.01 (GB2312 + ASCII subset).
 * Unifont is licensed under SIL OFL 1.1; see licenses/unifont-OFL-1.1.txt. */
const CjkGlyph kCjkGlyphs[] = {
""")
        for cp, width, bitmap in entries:
            vals = ",".join(f"0x{b:02x}" for b in bitmap)
            f.write(f"    {{0x{cp:04x}, {width}, {{{vals}}}}},\n")
        f.write("};\n\n")
        f.write("const int kCjkGlyphCount = (int)(sizeof(kCjkGlyphs) / sizeof(kCjkGlyphs[0]));\n")

    print(f"wrote {OUT_H}")
    print(f"wrote {OUT_CPP} ({os.path.getsize(OUT_CPP) // 1024} KiB)")

    # Optional visual sanity check.
    try:
        from PIL import Image
    except ImportError:
        print("Pillow not available, skipping preview")
        return

    text = "超级马里奥 中文测试 ABC 123"
    scale = 2
    glyph_map = {cp: (w, bytes(b)) for cp, w, b in entries}
    cursor = 4
    height = 16 * scale + 8
    positions = []
    for ch in text:
        cp = ord(ch)
        if cp not in glyph_map:
            cp = ord("?")
        w, bitmap = glyph_map[cp]
        positions.append((cursor, cp, w, bitmap))
        cursor += w * scale
    img = Image.new("RGB", (cursor + 4, height), (8, 10, 18))
    px = img.load()
    for x0, cp, w, bitmap in positions:
        for row in range(16):
            if row * 2 + 1 >= len(bitmap):
                break
            for col in range(w):
                if col < 8:
                    on = (bitmap[row * 2] >> (7 - col)) & 1
                else:
                    on = (bitmap[row * 2 + 1] >> (15 - col)) & 1
                if not on:
                    continue
                for sy in range(scale):
                    for sx in range(scale):
                        px[x0 + col * scale + sx, 4 + row * scale + sy] = (230, 235, 245)
    preview = os.path.join(ROOT, "tools", "cache", "font_preview.png")
    img.save(preview)
    print(f"wrote {preview}")


if __name__ == "__main__":
    main()
