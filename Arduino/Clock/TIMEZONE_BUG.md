# ESP32-C3 Deep Sleep Timezone Bug

## Status as of 2026-05-08

The earlier explanation in this file is no longer sufficient.

There was also a separate configuration mistake in the sketch: `timeZone` was set to `UTC-6`, which means **UTC+6** in POSIX notation. If the device is meant to run on Israel time, that setting will produce a constant hour error even when the RTC and NTP flow are otherwise working.

The sketch already does both of these:

- Calls `applyTimeZone()` (`setenv("TZ", ...)` + `tzset()`) at the top of every `setup()`
- Calls `configTzTime(timeZone, ntpServer)` only inside `ntpSync()` after Wi-Fi is connected

Despite that, the hour still alternates between two values across deep-sleep wakes.

That means "the `TZ` environment variable is lost during deep sleep" may still be true, but it does **not** fully explain the bug by itself. The timezone is now restored on every boot, and the issue still reproduces.

If the target location is Israel, the correct POSIX string is:

`IST-2IDT,M3.4.4/26,M10.5.0`

That corresponds to `Asia/Jerusalem`: UTC+2 in standard time and UTC+3 during daylight saving time.

## What Is Known

- Deep sleep wipes normal RAM, so `TZ` cannot be assumed to survive a wake.
- The code now restores `TZ` before the first `getLocalTime(&t)` call.
- Restoring `TZ` alone did not stop the alternating-hour behavior.
- The remaining issue is probably in the interaction between:
  - RTC epoch persistence
  - `getLocalTime()`
  - `localtime_r()`
  - `configTzTime()`
  - the NTP / non-NTP wake flow

## Current Working Hypotheses

1. The raw RTC epoch is correct, but local-time conversion is sometimes applied with stale or partially reset timezone state.
2. The raw RTC epoch itself is being rewritten or interpreted differently after alternating wake cycles.
3. `getLocalTime()` is not always returning the same view of time as `time()` + `localtime_r()`.
4. The sync decision path (`firstBoot`, `haveTime`, `ntpDay`) may be hiding the real transition point by making one cycle sync and the next cycle skip.

## Diagnostic Logging Added

`Clock.ino` now includes readable serial diagnostics around the entire time path:

- Boot number and wake cause
- `firstBoot` and `ntpDay`
- Previous pre-sleep snapshot stored in `RTC_DATA_ATTR`
- `TZ` before and after `applyTimeZone()`
- Raw `time_t` epoch
- `localtime_r()` output
- `gmtime_r()` output
- Initial `getLocalTime()` result
- NTP retry attempts
- State immediately after `configTzTime()`
- Final `getLocalTime()` result after NTP
- Final draw time and next sleep duration
- Final snapshot just before deep sleep

The key addition is the RTC-backed snapshot: even though the device enters deep sleep, the next boot can still print what the clock looked like immediately before sleeping.

## How To Read the Logs

Open Serial Monitor at `115200` and capture at least 3 consecutive cycles:

1. Cold boot with NTP sync
2. First deep-sleep wake
3. Second deep-sleep wake

Patterns to look for:

- If `epoch` keeps moving forward correctly but `localtime_r()` jumps by exactly the timezone offset, the bug is in timezone conversion/state.
- If both `gmtime_r()` and `localtime_r()` jump by the offset, the RTC epoch itself is moving.
- If `getLocalTime()` disagrees with `localtime_r(time())`, the problem is specifically in the `getLocalTime()` path.
- If the wrong value appears only after `configTzTime()`, the SNTP/timezone reconfiguration path becomes the main suspect.

## Next Step

Do not change the configured offset again until logs are collected from the new instrumentation.

The next useful artifact is a serial log that shows:

1. A successful NTP boot
2. The first wake after deep sleep
3. The second wake after deep sleep
4. The exact `epoch`, `gmtime_r()`, `localtime_r()`, and `getLocalTime()` values on each cycle
