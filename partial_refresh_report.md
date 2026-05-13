# E-Paper Partial Refresh Investigation Report

## Project Context

A Hebrew word clock built with a **Seeed Studio 7.5" e-paper display** (800×480, 1-bit black/white) driven by a **UC8179 controller**, connected to an **ESP32-C3 (XIAO)**. The clock wakes from deep sleep every minute, renders the current time in Hebrew words, updates the display, and goes back to sleep.

## Goal

Replace the displayed time text **without a full-screen black flash**. Currently, every time the minute changes, the entire display area flashes black before showing the new text — a distracting visual artifact, especially in a living room setting where the clock should update quietly.

## How E-Paper Refresh Works

E-paper pixels are microcapsules containing black and white charged particles. To move particles (change a pixel), voltage must be applied for a specific duration and polarity. The **waveform** defines the exact voltage sequence for each possible pixel transition (white→black, black→white, white→white, black→black).

The UC8179 controller supports two refresh modes:

### Full Refresh (FAST mode, register 0xE5 = 0x55)
- Drives every pixel through a complete **black → white → black → target** cycle
- Guarantees every pixel reaches its correct final state
- **The black flash IS the waveform** — it is not a side effect, it is the mechanism itself
- Result: perfectly clean display, but visually disruptive

### Partial Refresh (PARTIAL mode, register 0xE5 = 0x6E)
- Uses a weaker, shorter waveform that attempts to transition pixels directly to their target state
- No intermediate black flash
- But the weaker waveform **does not fully drive pixels** — especially the black→white transition (erasing old text) is incomplete
- Result: new text appears, but old text leaves a visible gray ghost (ghosting)

## The Fundamental Problem

Erasing black pixels (old text) to white requires significant electrical energy to physically move the ink particles. The partial refresh waveform does not provide enough energy for this transition. **The ghosting is not a software bug — it is a physical limitation of the waveform strength.**

There is no register setting, buffer manipulation, or software technique that can make the partial waveform fully erase black pixels, because the waveform lookup tables (LUTs) are burned into the controller's OTP (One-Time Programmable) memory at the factory. We can only select between the pre-programmed LUTs via the temperature register (0xE5), not create custom ones.

## Attempts Made and Why They Failed

### Attempt 1: Change the Waveform Strength (0xE5 value)

**What we did:** Changed the partial refresh LUT selection register from 0x6E to 0x28 (a much more aggressive waveform).

**Result:** No visible change.

**Why it failed:** The 0xE5 register on UC8179 selects a LUT based on a temperature value. The available LUTs in OTP are limited. Value 0x28 likely maps to a LUT that is not meaningfully different for the black→white transition, or the controller clamps to the nearest valid LUT.

### Attempt 2: Send the Old Buffer (Differential Update)

**What we did:** The UC8179 accepts two image buffers:
- Register 0x10 (DTM1): the "old" image (what's currently on screen)
- Register 0x13 (DTM2): the "new" image (what to display)

The controller computes per-pixel transitions based on old→new state. The original `updataPartial()` only sent the new buffer (0x13), never the old (0x10). We added code to reconstruct the old image from saved text lines (stored in RTC memory across deep sleep) and send it as the old buffer.

**Result:** Severe visual artifacts — overlapping text, corrupted display.

**Why it failed:** After deep sleep, the ESP32 restarts completely. The 48KB framebuffer cannot be preserved in RTC memory (only 8KB available on ESP32-C3). We reconstructed the old image by re-rendering the previous text, but even tiny pixel-level differences between the reconstruction and what's actually on screen cause the controller to compute incorrect transitions. The differential update requires a **pixel-perfect** copy of what's on the display, which we cannot guarantee after deep sleep.

### Attempt 3: Fix the Waveform Mode Bug (EPD_INIT_PARTIAL)

**What we discovered:** `epaper.begin()` calls `EPD_WAKEUP()` which initializes the controller with `EPD_INIT_FAST()` (register 0xE5 = 0x55, the full-refresh waveform). Then `updataPartial()` checks if the controller is sleeping — but since `begin()` already woke it, the check is false, and `EPD_INIT_PARTIAL()` is **never called**. The partial refresh was actually running with the full-refresh waveform all along.

**What we did:** Added `EPD_INIT_PARTIAL()` in the else-branch so the partial waveform registers are always set before a partial update.

**Result:** The flash was eliminated, confirming the fix worked. But ghosting appeared — old text remains as a gray shadow. This is the expected behavior of the partial waveform: less flash, but insufficient driving force for clean pixel transitions.

**Important discovery:** The original "full black flash during partial refresh" was caused by this bug — the controller was using the FAST waveform (which always flashes) instead of the PARTIAL waveform. Fixing the bug revealed the true partial refresh behavior: no flash, but ghosting.

### Attempt 4: Double Partial Refresh (Clear Then Draw)

**What we did:** Instead of one partial refresh, performed two in sequence:
1. Fill the region with white → partial refresh (push old text pixels toward white)
2. Draw new text → partial refresh (render new content on cleaner background)

The idea: old text pixels get two opportunities to be driven toward white.

**Result:** Still visible ghosting after both passes.

**Why it failed:** Each partial refresh pass applies the same weak waveform. If one pass moves a black pixel to ~70% gray, the second pass (seeing a ~70% gray pixel being asked to stay white) applies an even weaker white→white transition. The cumulative effect is marginal.

### Attempt 5: Triple Partial Refresh

**What we did:** Three passes — two white clears followed by one text draw.

**Result:** Still visible ghosting.

**Why it failed:** Same reason as attempt 4. The partial waveform is designed for pixels that are already near their target state (minor corrections). It cannot accumulate enough energy across multiple passes to fully drive a black pixel to white, because each subsequent pass sees a lighter pixel and applies proportionally less driving force.

## Technical Root Cause Summary

The ghosting problem has three reinforcing causes:

1. **Weak partial waveform:** The partial refresh LUT in the UC8179's OTP does not provide sufficient voltage/duration to fully transition black→white pixels. This is by design — a stronger waveform would cause visible flashing, defeating the purpose of partial refresh.

2. **No old buffer across deep sleep:** The UC8179 can compute optimal per-pixel transitions when given both old and new images, but after ESP32 deep sleep the old framebuffer is lost. RTC memory is too small (8KB) to preserve the 48KB framebuffer. Reconstructing from saved text introduces pixel-level inaccuracies.

3. **Factory-locked LUTs:** The waveform lookup tables are in OTP memory and cannot be modified. We can only select between pre-programmed LUTs via the temperature register, not define custom waveforms with stronger black→white transitions.

## Conclusion

On this hardware (UC8179 controller + 7.5" Seeed e-paper), **flash-free text updates without ghosting are not achievable through software alone**. The display's partial refresh capability is designed for minor screen corrections (e.g., cursor blink, small UI changes), not for replacing large text content.

The available options are:
- **Full refresh with flash** (current behavior): clean text, but visually disruptive black flash
- **Partial refresh without flash**: ghosting makes previous text visible behind new text
- **No refresh when text unchanged**: skip the update entirely when the displayed time phrase hasn't changed (already implemented, saves unnecessary flashes)
