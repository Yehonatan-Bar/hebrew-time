# Changing the Refresh Interval

The board wakes from deep sleep on a timer to update the displayed time. Two constants in `Arduino/Clock/Clock.ino` control how often:

```cpp
#define SLEEP_FAST_SEC   120   // normal hours (seconds between refreshes)
#define SLEEP_SLOW_SEC   300   // quiet hours  (seconds between refreshes)
```

## Which one applies when?

The function `isLowFrequencyTime()` decides. When it returns `true`, the board uses `SLEEP_SLOW_SEC`; otherwise it uses `SLEEP_FAST_SEC`.

Current quiet-hour windows (use the slow interval):

| Window | Days |
|---|---|
| 00:00 - 06:00 | Every day |
| 09:00 - 13:00 | Sun - Thu |

Everything else uses the fast interval.

## How to change

1. Open `Arduino/Clock/Clock.ino`.
2. Edit `SLEEP_FAST_SEC` and/or `SLEEP_SLOW_SEC` to the desired number of seconds.
3. To change *when* quiet hours apply, edit the conditions inside `isLowFrequencyTime()`.
4. Compile and upload.

## Example

To refresh every 3 minutes during normal hours and every 10 minutes during quiet hours:

```cpp
#define SLEEP_FAST_SEC   180
#define SLEEP_SLOW_SEC   600
```

## Things to keep in mind

- The board does a full NTP sync once per day (on first boot of the day). Between syncs the ESP32 RTC keeps time, so very long sleep intervals won't cause clock drift within a single day.
- Shorter intervals mean more e-ink refreshes, which slightly increases wear and power consumption.
- Partial refresh (`updataPartial`) is used on every wake except the very first boot, so flicker is minimal regardless of interval.
