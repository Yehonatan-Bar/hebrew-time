# Changing the Hebrew Font

This guide explains how to replace the Hebrew font used by the word clock.

## Prerequisites

- Python 3 with the `freetype-py` package (`pip install freetype-py`)
- A `.ttf` font file that includes Hebrew glyphs (U+05D0-U+05EA)
- The font converter script at `tools/ttf_to_gfx.py`

## Step 1: Obtain the font file

Place the `.ttf` file in the `tools/` directory. Variable fonts (e.g. `Font[wght].ttf`) are supported — the converter automatically sets the weight axis to 700 (Bold).

Google Fonts is a good source. To download from their GitHub repo:

```
curl -L -o tools/MyFont.ttf "https://github.com/google/fonts/raw/main/ofl/myfontname/MyFont%5Bwght%5D.ttf"
```

Verify the download is actually a font file, not an HTML redirect:

```
file tools/MyFont.ttf
# Should say "TrueType Font data", NOT "HTML document"
```

## Step 2: Generate the GFX header

Run the converter:

```
python tools/ttf_to_gfx.py tools/MyFont.ttf 85 "C:/Users/User/Documents/Arduino/libraries/Seeed_GFX/Fonts/Custom/MyFont_Bold_85.h" MyFont_Bold_85
```

Arguments:
1. Path to the `.ttf` file
2. Font size in pixels (currently 85 — adjust to taste, but update `FONT_BASE_H` in `Clock.ino` to match)
3. Output path — must be inside `Seeed_GFX/Fonts/Custom/`
4. C identifier name for the font (used in code)

The converter generates glyphs for ASCII 0x20-0x7E and Hebrew 0x05B0-0x05EA as 1-bit (mono) bitmaps.

## Step 3: Update Clock.ino

Two places reference the font by name:

1. **The include** (near the top):
   ```cpp
   #include <Fonts/Custom/MyFont_Bold_85.h>
   ```

2. **The font pointer** (in the draw function, search for `GFXfont* font`):
   ```cpp
   const GFXfont* font = &MyFont_Bold_85;
   ```

If you changed the pixel size, also update:
```cpp
#define FONT_BASE_H  85  // must match the size passed to ttf_to_gfx.py
```

## Step 4: Compile and upload

```powershell
# Compile
& "C:\Users\User\AppData\Local\Programs\Arduino IDE\resources\app\lib\backend\resources\arduino-cli.exe" compile --fqbn espressif:esp32:XIAO_ESP32C3 "c:\projects\smart_board\Arduino\Clock\Clock.ino"

# Upload (adjust COM port)
& "C:\Users\User\AppData\Local\Programs\Arduino IDE\resources\app\lib\backend\resources\arduino-cli.exe" upload -p COM5 --fqbn espressif:esp32:XIAO_ESP32C3 "c:\projects\smart_board\Arduino\Clock\Clock.ino"
```

## Notes

- The ESP32-C3 has 1.3MB flash for the sketch. At 85px the font header is ~40-50KB, so there's room, but very large sizes (150px+) may push close to the limit.
- The e-ink screen is 800x480. Font sizes above ~100px will cause long words (e.g. "שלושים ושמונה") to overflow the screen width.
- The converter renders in mono mode (`FT_LOAD_TARGET_MONO`) which gives the sharpest result on e-ink. Grayscale rendering is not suitable for this 1-bit display.
- For variable fonts, the converter sets weight to 700 (Bold). To change this, edit the `700` value in `ttf_to_gfx.py` at the `FT_Set_Var_Design_Coordinates` call.
