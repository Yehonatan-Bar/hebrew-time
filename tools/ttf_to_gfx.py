"""Convert a TTF font to Adafruit GFXfont .h header for TFT_eSPI.

Usage:
    python ttf_to_gfx.py <font.ttf> <size_px> <output.h> <font_name>

Generates glyphs for ASCII 0x20-0x7E and Hebrew 0x05B0-0x05EA.
Each glyph bitmap is padded to byte boundary (GFXfont requirement).
"""

import sys
import freetype

def main():
    ttf_path  = sys.argv[1]
    size_px   = int(sys.argv[2])
    out_path  = sys.argv[3]
    font_name = sys.argv[4]

    face = freetype.Face(ttf_path)
    face.set_pixel_sizes(0, size_px)

    try:
        coords = freetype.FT_Fixed * 1
        c = coords()
        c[0] = freetype.FT_Fixed(int(700 * 65536))
        freetype.FT_Set_Var_Design_Coordinates(face._FT_Face, 1, c)
        print(f"  Variable font: weight set to 700 (Bold)")
    except:
        pass

    wanted = set()
    wanted.update(range(0x20, 0x7F))
    wanted.update(range(0x05B0, 0x05EB))

    first_code = min(wanted)
    last_code  = max(wanted)

    bitmaps = bytearray()
    glyphs  = []

    for cp in range(first_code, last_code + 1):
        byte_offset = len(bitmaps)
        if cp in wanted:
            face.load_char(chr(cp), freetype.FT_LOAD_RENDER | freetype.FT_LOAD_TARGET_MONO)
            bmp = face.glyph.bitmap
            w = bmp.width
            h = bmp.rows
            xa = face.glyph.advance.x >> 6
            xo = face.glyph.bitmap_left
            yo = -face.glyph.bitmap_top

            # Pack w*h bits, then pad to next byte boundary
            bit_count = 0
            for row in range(h):
                row_start = row * bmp.pitch
                for col in range(w):
                    src_byte = col >> 3
                    src_bit  = 7 - (col & 7)
                    pixel = (bmp.buffer[row_start + src_byte] >> src_bit) & 1

                    dst_bit = 7 - (bit_count & 7)
                    if dst_bit == 7:
                        bitmaps.append(0)
                    if pixel:
                        bitmaps[-1] |= (1 << dst_bit)
                    bit_count += 1

            # Pad remaining bits in last byte (already zeros)
            # Just ensure we ended a byte
            if bit_count & 7:
                pass  # last byte already appended, partial bits are zero-filled
        else:
            w, h, xa, xo, yo = 0, 0, 0, 0, 0

        glyphs.append((byte_offset, w, h, xa, xo, yo))

    with open(out_path, 'w', encoding='utf-8') as f:
        f.write(f"// {font_name} — auto-generated from {ttf_path.split('/')[-1].split(chr(92))[-1]}, {size_px}px\n")
        f.write("#pragma once\n")
        f.write('#include "../GFXFF/gfxfont.h"\n\n')

        f.write(f"const uint8_t {font_name}Bitmaps[] PROGMEM = {{\n")
        for i, b in enumerate(bitmaps):
            if i % 16 == 0:
                f.write("  ")
            f.write(f"0x{b:02X}")
            if i < len(bitmaps) - 1:
                f.write(", ")
            if i % 16 == 15:
                f.write("\n")
        f.write("\n};\n\n")

        f.write(f"const GFXglyph {font_name}Glyphs[] PROGMEM = {{\n")
        for i, (offset, w, h, xa, xo, yo) in enumerate(glyphs):
            cp = first_code + i
            if 0x20 <= cp < 0x7F:
                ch = chr(cp)
            elif cp >= 0x05B0:
                ch = f"U+{cp:04X}"
            else:
                ch = "gap"
            f.write(f"  {{ {offset:6d}, {w:3d}, {h:3d}, {xa:3d}, {xo:4d}, {yo:4d} }}")
            if i < len(glyphs) - 1:
                f.write(",")
            f.write(f"  // 0x{cp:04X} '{ch}'\n")
        f.write("};\n\n")

        f.write(f"const GFXfont {font_name} PROGMEM = {{\n")
        f.write(f"  (uint8_t  *){font_name}Bitmaps,\n")
        f.write(f"  (GFXglyph *){font_name}Glyphs,\n")
        f.write(f"  0x{first_code:04X}, 0x{last_code:04X}, {size_px}\n")
        f.write("};\n")

    real_glyphs = sum(1 for cp in range(first_code, last_code+1) if cp in wanted)
    print(f"Generated {out_path}")
    print(f"  {len(bitmaps)} bitmap bytes, {real_glyphs} real glyphs")

if __name__ == "__main__":
    main()
