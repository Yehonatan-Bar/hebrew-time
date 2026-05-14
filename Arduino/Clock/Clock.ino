// Smart Board — Hebrew word clock
//
// Displays the current time in Hebrew words
// on a 7.5" e-ink display (Seeed XIAO e-paper driver, model 502).
//
// Update cadence: every 1 minute
//
// SETUP NOTE — partial refresh:
//   To enable partial-refresh updates, add this line to your library setup
//   file (e.g. libraries/Seeed_GFX/User_Setups/Setup502_Seeed_XIAO_EPaper_7inch5.h):
//
//     #define USE_PARTIAL_EPAPER
//
//   Without it, the call to epaper.updataPartial() below won't compile.
//   (Yes, "updata" — that's the actual name in the library.)
//
#include "driver.h"
#include <TFT_eSPI.h>
#include <SPI.h>
#include <WiFi.h>
#include <time.h>
#include <sys/time.h>
#include <stdlib.h>
#include "esp_sleep.h"
#include "esp_sntp.h"
#include <Fonts/Custom/NotoSerifHebrew_Bold_85.h>
#include "time_words.h"
#include "secrets.h"

#ifdef EPAPER_ENABLE

EPaper epaper = EPaper();

// ── Wi-Fi (used briefly for scheduled NTP syncs) ──
const char* ssid     = WIFI_SSID;
const char* password = WIFI_PASSWORD;

// ── NTP ───────────────────────────────────────────────
const char* ntpServer          = "pool.ntp.org";
const char* timeZone           = "IST-2IDT,M3.4.4/26,M10.5.0"; // Israel: UTC+2 standard, UTC+3 during DST
const bool  ENABLE_TIME_DEBUG  = true;

// ── Sleep / refresh schedule ──────────────────────────
#define SLEEP_FAST_SEC   60
#define SLEEP_SLOW_SEC   300
#define WIFI_TIMEOUT     20
#define TIME_OFFSET_SEC  60   // keep the displayed time one minute ahead
#define FULL_REFRESH_EVERY 15 // full refresh every ~15 partial cycles to clear ghosting

// ── Layout ────────────────────────────────────────────
#define SCREEN_W         800
#define SCREEN_H         480
#define TEXT_SCALE       1
#define FONT_BASE_H      85
#define HEBREW_SPACE_W   20
#define LINE_GAP         24
#define FONT_ASCENT      55
#define FONT_DESCENT      7

// Rectangle that gets cleared & redrawn (must satisfy 8-px X alignment for partial refresh)
#define TIME_BOX_X       0
#define TIME_BOX_Y       60
#define TIME_BOX_W       800
#define TIME_BOX_H       400

#define MAX_LINES 3

// ── State preserved across deep-sleep cycles ──────────
RTC_DATA_ATTR int    ntpDay    = -1;
RTC_DATA_ATTR int    ntpSlot   = -1;
RTC_DATA_ATTR int    bootCount = 0;
RTC_DATA_ATTR bool   firstBoot = true;
RTC_DATA_ATTR time_t savedEpoch = 0;
RTC_DATA_ATTR int    savedSleepSec = 0;
RTC_DATA_ATTR char   prevLines[MAX_LINES][64];
RTC_DATA_ATTR int    prevLineCount = 0;
RTC_DATA_ATTR int    partialCount = 0;
RTC_DATA_ATTR bool   epdRamValid = false;

struct TimeDebugSnapshot {
  time_t epoch;
  int    localYday;
  int    localHour;
  int    localMin;
  int    utcYday;
  int    utcHour;
  int    utcMin;
  int    ntpDay;
  int    ntpSlot;
  int    bootCount;
  bool   firstBoot;
};

RTC_DATA_ATTR TimeDebugSnapshot lastSleepSnapshot;
RTC_DATA_ATTR bool              lastSleepSnapshotValid = false;

// ── Types used by Hebrew RTL + niqud rendering ───────
struct GlyphMetrics {
  uint8_t  width;
  uint8_t  height;
  uint8_t  xAdvance;
  int8_t   xOffset;
  int8_t   yOffset;
};

// ── Forward declarations ──────────────────────────────
void   applyTimeZone();
bool   ntpSync(struct tm* syncedTime);
void   drawTimeInWords(const struct tm& t, bool fullRefresh);
void   drawCenteredLines(const String& l1, const String& l2, const String& l3, int numLines);
void   splitTimePhrase(const struct tm& t, String& line1, String& line2, String& line3);
void   drawError(const String& msg);
void   goToSleep(int seconds);
bool   isLowFrequencyTime(const struct tm& t);
int    sleepSeconds(const struct tm& t);
int    scheduledNtpSlot(const struct tm& t);
bool   shouldRunScheduledNtp(const struct tm& t);
const char* wakeCauseName(esp_sleep_wakeup_cause_t cause);
void   logTm(const char* label, const struct tm& t);
void   logClockState(const char* label);
void   logGetLocalTimeResult(const char* label, bool ok, const struct tm* t);
void   storeSleepSnapshot();
void   printSleepSnapshot();

// ──────────────────────────────────────────────────────
//  setup() — entry point on every wake
// ──────────────────────────────────────────────────────
void setup() {
  Serial.begin(115200);
  delay(300);

  bootCount++;

  // ── Diagnostic: check if ESP32 RTC maintains time across deep sleep ──
  time_t rtcEpoch = 0;
  time(&rtcEpoch);
  int64_t bootMicros = esp_timer_get_time();
  time_t manualEpoch = savedEpoch + savedSleepSec;
  Serial.printf(
    "=== CLOCK DRIFT DIAG ===\n"
    "  RTC auto epoch   = %lld (what the HW clock says)\n"
    "  Manual restore   = %lld (savedEpoch %lld + sleep %d)\n"
    "  Difference       = %lld sec (positive = RTC is ahead)\n"
    "  Boot elapsed     = %lld us\n"
    "========================\n",
    (long long)rtcEpoch,
    (long long)manualEpoch,
    (long long)savedEpoch,
    savedSleepSec,
    (long long)(rtcEpoch - manualEpoch),
    (long long)bootMicros
  );

  Serial.printf(
    "Boot #%d wakeCause=%s(%d) firstBoot=%d ntpDay=%d ntpSlot=%d\n",
    bootCount,
    wakeCauseName(esp_sleep_get_wakeup_cause()),
    (int)esp_sleep_get_wakeup_cause(),
    firstBoot,
    ntpDay,
    ntpSlot
  );
  printSleepSnapshot();
  logClockState("boot/before applyTimeZone");

  bool retainedWake = !firstBoot && epdRamValid &&
                      esp_sleep_get_wakeup_cause() == ESP_SLEEP_WAKEUP_TIMER;
  if (retainedWake) {
    epaper.beginRetainedPowerOff();
  } else {
    epaper.begin(0);
    epdRamValid = false;
  }
  epaper.setRotation(0);

  // Restore timezone on every boot without restarting SNTP.
  applyTimeZone();

  // Restore system clock from RTC memory so we don't need NTP every boot.
  if (savedEpoch > 0 && !firstBoot) {
    time_t restored = savedEpoch + savedSleepSec;
    struct timeval tv = { .tv_sec = restored, .tv_usec = 0 };
    settimeofday(&tv, nullptr);
    Serial.printf("Restored time from RTC memory: epoch=%lld + sleep=%d\n",
                  (long long)savedEpoch, savedSleepSec);
  }
  logClockState("boot/after applyTimeZone");

  struct tm t;
  bool haveTime = getLocalTime(&t);
  logGetLocalTimeResult("boot/initial", haveTime, haveTime ? &t : nullptr);
  bool scheduledSyncDue = haveTime && shouldRunScheduledNtp(t);
  bool needNTP          = firstBoot || !haveTime || scheduledSyncDue;
  Serial.printf(
    "needNTP=%d (firstBoot=%d haveTime=%d scheduledSyncDue=%d tm_yday=%d ntpDay=%d ntpSlot=%d targetSlot=%d)\n",
    needNTP,
    firstBoot,
    haveTime,
    scheduledSyncDue,
    haveTime ? t.tm_yday : -1,
    ntpDay,
    ntpSlot,
    haveTime ? scheduledNtpSlot(t) : -1
  );

  if (needNTP) {
    bool ntpSuccess = false;
    for (int attempt = 0; attempt < 3; attempt++) {
      Serial.printf("NTP attempt %d/3\n", attempt + 1);
      ntpSuccess = ntpSync(&t);
      logGetLocalTimeResult("boot/post-ntp", ntpSuccess, ntpSuccess ? &t : nullptr);
      logClockState("boot/post-ntp");
      if (ntpSuccess) break;
      delay(2000);
    }
    if (!ntpSuccess && !haveTime) {
      logClockState("boot/ntp-failed");
      drawError(String("NTP failed - retrying in ") + SLEEP_FAST_SEC + "s");
      goToSleep(SLEEP_FAST_SEC);
      return;
    }
    if (ntpSuccess) {
      ntpDay  = t.tm_yday;
      ntpSlot = scheduledNtpSlot(t);
      Serial.printf("Updated ntpDay=%d ntpSlot=%d\n", ntpDay, ntpSlot);
    } else {
      Serial.println("Scheduled NTP sync failed - keeping restored time");
      if (!getLocalTime(&t)) {
        logClockState("boot/ntp-failed");
        drawError(String("Time unavailable - retrying in ") + SLEEP_FAST_SEC + "s");
        goToSleep(SLEEP_FAST_SEC);
        return;
      }
    }
  }

  // Apply manual time offset
  time_t adjusted = mktime(&t) + TIME_OFFSET_SEC;
  localtime_r(&adjusted, &t);

  logTm("boot/final local", t);
  int sleepSec = sleepSeconds(t);
  Serial.printf("Scheduling deep sleep for %d seconds\n", sleepSec);
  drawTimeInWords(t, firstBoot);
  firstBoot = false;

  goToSleep(sleepSec);
}

void loop() {}

void applyTimeZone() {
  if (ENABLE_TIME_DEBUG) {
    const char* oldTz = getenv("TZ");
    Serial.printf(
      "applyTimeZone: old TZ=%s requested TZ=%s\n",
      oldTz ? oldTz : "(null)",
      timeZone
    );
  }
  setenv("TZ", timeZone, 1);
  tzset();
  if (ENABLE_TIME_DEBUG) {
    const char* newTz = getenv("TZ");
    Serial.printf("applyTimeZone: effective TZ=%s\n", newTz ? newTz : "(null)");
  }
}

const char* wakeCauseName(esp_sleep_wakeup_cause_t cause) {
  switch (cause) {
    case ESP_SLEEP_WAKEUP_UNDEFINED: return "undefined";
    case ESP_SLEEP_WAKEUP_EXT0:      return "ext0";
    case ESP_SLEEP_WAKEUP_EXT1:      return "ext1";
    case ESP_SLEEP_WAKEUP_TIMER:     return "timer";
    case ESP_SLEEP_WAKEUP_TOUCHPAD:  return "touchpad";
    case ESP_SLEEP_WAKEUP_ULP:       return "ulp";
    case ESP_SLEEP_WAKEUP_GPIO:      return "gpio";
    case ESP_SLEEP_WAKEUP_UART:      return "uart";
    default:                         return "unknown";
  }
}

void logTm(const char* label, const struct tm& t) {
  if (!ENABLE_TIME_DEBUG) return;
  Serial.printf(
    "%s: %04d-%02d-%02d %02d:%02d:%02d yday=%d wday=%d isdst=%d\n",
    label,
    t.tm_year + 1900,
    t.tm_mon + 1,
    t.tm_mday,
    t.tm_hour,
    t.tm_min,
    t.tm_sec,
    t.tm_yday,
    t.tm_wday,
    t.tm_isdst
  );
}

void logClockState(const char* label) {
  if (!ENABLE_TIME_DEBUG) return;

  time_t now = 0;
  time(&now);

  const char* tz = getenv("TZ");
  Serial.printf("[%s] TZ=%s epoch=%lld\n", label, tz ? tz : "(null)", (long long)now);

  struct tm localTm;
  if (localtime_r(&now, &localTm)) {
    logTm("  localtime_r", localTm);
  } else {
    Serial.println("  localtime_r failed");
  }

  struct tm utcTm;
  if (gmtime_r(&now, &utcTm)) {
    logTm("  gmtime_r", utcTm);
  } else {
    Serial.println("  gmtime_r failed");
  }
}

void logGetLocalTimeResult(const char* label, bool ok, const struct tm* t) {
  if (!ENABLE_TIME_DEBUG) return;
  Serial.printf("%s: getLocalTime=%s\n", label, ok ? "ok" : "failed");
  if (ok && t) logTm("  getLocalTime", *t);
}

void storeSleepSnapshot() {
  if (!ENABLE_TIME_DEBUG) return;

  time_t now = 0;
  time(&now);

  struct tm localTm = {};
  struct tm utcTm   = {};
  localtime_r(&now, &localTm);
  gmtime_r(&now, &utcTm);

  lastSleepSnapshot.epoch     = now;
  lastSleepSnapshot.localYday = localTm.tm_yday;
  lastSleepSnapshot.localHour = localTm.tm_hour;
  lastSleepSnapshot.localMin  = localTm.tm_min;
  lastSleepSnapshot.utcYday   = utcTm.tm_yday;
  lastSleepSnapshot.utcHour   = utcTm.tm_hour;
  lastSleepSnapshot.utcMin    = utcTm.tm_min;
  lastSleepSnapshot.ntpDay    = ntpDay;
  lastSleepSnapshot.ntpSlot   = ntpSlot;
  lastSleepSnapshot.bootCount = bootCount;
  lastSleepSnapshot.firstBoot = firstBoot;
  lastSleepSnapshotValid      = true;
}

void printSleepSnapshot() {
  if (!ENABLE_TIME_DEBUG || !lastSleepSnapshotValid) return;

  Serial.printf(
    "Prev sleep snapshot: boot=%d epoch=%lld local=yday %d %02d:%02d utc=yday %d %02d:%02d ntpDay=%d ntpSlot=%d firstBoot=%d\n",
    lastSleepSnapshot.bootCount,
    (long long)lastSleepSnapshot.epoch,
    lastSleepSnapshot.localYday,
    lastSleepSnapshot.localHour,
    lastSleepSnapshot.localMin,
    lastSleepSnapshot.utcYday,
    lastSleepSnapshot.utcHour,
    lastSleepSnapshot.utcMin,
    lastSleepSnapshot.ntpDay,
    lastSleepSnapshot.ntpSlot,
    lastSleepSnapshot.firstBoot
  );
}

// ──────────────────────────────────────────────────────
//  Sleep mode logic
// ──────────────────────────────────────────────────────
bool isLowFrequencyTime(const struct tm& t) {
  // tm_wday: 0=Sun .. 6=Sat
  if (t.tm_wday >= 0 && t.tm_wday <= 4 && t.tm_hour >= 9 && t.tm_hour < 13) return true;  // Sun-Thu 09:00-13:00
  if (t.tm_hour < 6) return true;                                                          // every day 00:00-06:00
  return false;
}

int scheduledNtpSlot(const struct tm& t) {
  if (t.tm_hour >= 14) return 1;
  if (t.tm_hour >= 6) return 0;
  return -1;
}

bool shouldRunScheduledNtp(const struct tm& t) {
  int targetSlot = scheduledNtpSlot(t);
  if (targetSlot < 0) return false;
  if (ntpDay != t.tm_yday) return true;
  return ntpSlot < targetSlot;
}

bool isSpecialMinute(int m) {
  return m == 0 || m == 5 || m == 10 || m == 15 || m == 30 ||
         m == 40 || m == 45 || m == 50 || m == 55;
}

int sleepSeconds(const struct tm& t) {
  int nextMin = (t.tm_min + 1) % 60;
  if (isSpecialMinute(nextMin)) return 60;
  return SLEEP_FAST_SEC;
}

// ──────────────────────────────────────────────────────
//  NTP sync — turn WiFi on briefly, sync, turn off
// ──────────────────────────────────────────────────────
bool ntpSync(struct tm* syncedTime) {
  Serial.println("NTP sync...");
  logClockState("ntpSync/before WiFi");
  WiFi.begin(ssid, password);
  int tries = 0;
  while (WiFi.status() != WL_CONNECTED && tries < WIFI_TIMEOUT) { delay(500); tries++; }
  Serial.printf("WiFi status=%d after %d waits\n", WiFi.status(), tries);
  if (WiFi.status() == WL_CONNECTED) {
    Serial.printf("Calling configTzTime(TZ=%s, server=%s)\n", timeZone, ntpServer);
    sntp_restart();
    configTzTime(timeZone, ntpServer);
    logClockState("ntpSync/after configTzTime");
    int ntpTries = 0;
    bool synced = false;
    while (ntpTries < 15) {
      if (sntp_get_sync_status() == SNTP_SYNC_STATUS_COMPLETED) {
        synced = true;
        break;
      }
      ntpTries++;
      Serial.printf("Waiting for NTP sync... %d/15\n", ntpTries);
      delay(1000);
    }
    struct tm t;
    bool gotTime = synced && getLocalTime(&t);
    logGetLocalTimeResult("ntpSync/final", gotTime, gotTime ? &t : nullptr);
    logClockState("ntpSync/final");
    Serial.printf("NTP sync %s after %d seconds\n", synced ? "completed" : "FAILED", ntpTries);
    if (gotTime && syncedTime) *syncedTime = t;
    WiFi.disconnect(true);
    WiFi.mode(WIFI_OFF);
    return gotTime;
  } else {
    Serial.println("WiFi failed — keeping previous time");
  }
  if (WiFi.status() != WL_CONNECTED) {
    logClockState("ntpSync/WiFi-failed");
  }
  WiFi.disconnect(true);
  WiFi.mode(WIFI_OFF);
  return false;
}

// ──────────────────────────────────────────────────────
//  Hebrew RTL + niqud support.
//  GFXfont can't position combining marks (niqud) correctly
//  because it lacks OpenType GPOS support. We draw glyphs
//  individually and center each niqud mark on its base letter.
// ──────────────────────────────────────────────────────
bool isNiqudCP(uint16_t cp) {
  return (cp >= 0x05B0 && cp <= 0x05BD) || cp == 0x05BF ||
         cp == 0x05C1 || cp == 0x05C2 || cp == 0x05C7;
}

bool isNikudByte(uint8_t b0, uint8_t b1) {
  uint16_t cp = ((b0 & 0x1F) << 6) | (b1 & 0x3F);
  return isNiqudCP(cp);
}

uint16_t decodeUTF8(const String& s, int& pos) {
  uint8_t b0 = (uint8_t)s[pos];
  if ((b0 & 0xE0) == 0xC0 && pos + 1 < (int)s.length()) {
    uint8_t b1 = (uint8_t)s[pos + 1];
    pos += 2;
    return ((b0 & 0x1F) << 6) | (b1 & 0x3F);
  }
  pos++;
  return b0;
}

void drawGlyphBitmap(const GFXfont* font, uint16_t cp, int x, int y) {
  if (cp < pgm_read_word(&font->first) || cp > pgm_read_word(&font->last)) return;
  const GFXglyph* glyph = &((const GFXglyph*)pgm_read_ptr(&font->glyph))[cp - pgm_read_word(&font->first)];
  const uint8_t* bitmap = (const uint8_t*)pgm_read_ptr(&font->bitmap);

  uint32_t bo = pgm_read_dword(&glyph->bitmapOffset);
  uint8_t  w  = pgm_read_byte(&glyph->width);
  uint8_t  h  = pgm_read_byte(&glyph->height);

  int bit = 0;
  for (int row = 0; row < h; row++) {
    for (int col = 0; col < w; col++) {
      if (pgm_read_byte(&bitmap[bo + bit / 8]) & (0x80 >> (bit & 7)))
        epaper.drawPixel(x + col, y + row, TFT_BLACK);
      bit++;
    }
  }
}

GlyphMetrics getGlyphMetrics(const GFXfont* font, uint16_t cp) {
  GlyphMetrics m = {0, 0, 0, 0, 0};
  if (cp < pgm_read_word(&font->first) || cp > pgm_read_word(&font->last)) return m;
  const GFXglyph* glyph = &((const GFXglyph*)pgm_read_ptr(&font->glyph))[cp - pgm_read_word(&font->first)];
  m.width    = pgm_read_byte(&glyph->width);
  m.height   = pgm_read_byte(&glyph->height);
  m.xAdvance = pgm_read_byte(&glyph->xAdvance);
  m.xOffset  = (int8_t)pgm_read_byte(&glyph->xOffset);
  m.yOffset  = (int8_t)pgm_read_byte(&glyph->yOffset);
  return m;
}

int drawHebrewWord(const GFXfont* font, const String& word, int x, int y) {
  int cursor = x;
  int lastBaseX = x;
  int lastBaseXOff = 0;
  int lastBaseW = 0;

  int i = 0;
  while (i < (int)word.length()) {
    uint16_t cp = decodeUTF8(word, i);
    GlyphMetrics m = getGlyphMetrics(font, cp);

    if (isNiqudCP(cp)) {
      int markX;
      if (cp == 0x05C1)        // SHIN DOT — right side of letter
        markX = lastBaseX + lastBaseXOff + lastBaseW - m.width;
      else if (cp == 0x05C2 || cp == 0x05B9)  // SIN DOT / HOLAM — left side
        markX = lastBaseX + lastBaseXOff;
      else                     // all other niqud — centered
        markX = lastBaseX + lastBaseXOff + lastBaseW / 2 - m.width / 2;
      int markY = y + m.yOffset;
      drawGlyphBitmap(font, cp, markX, markY);
    } else {
      drawGlyphBitmap(font, cp, cursor + m.xOffset, y + m.yOffset);
      lastBaseX = cursor;
      lastBaseXOff = m.xOffset;
      lastBaseW = m.width;
      cursor += m.xAdvance;
    }
  }
  return cursor - x;
}

int measureHebrewWord(const GFXfont* font, const String& word) {
  int width = 0;
  int i = 0;
  while (i < (int)word.length()) {
    uint16_t cp = decodeUTF8(word, i);
    if (!isNiqudCP(cp))
      width += getGlyphMetrics(font, cp).xAdvance;
  }
  return width;
}

String reverseHebrew(const String& word) {
  String clusters[32];
  int count = 0;
  int i = 0;
  while (i < (int)word.length() && count < 32) {
    uint8_t c = (uint8_t)word[i];
    if ((c & 0xE0) == 0xC0 && i + 1 < (int)word.length()) {
      clusters[count] = word.substring(i, i + 2);
      i += 2;
      while (i + 1 < (int)word.length() &&
             isNikudByte((uint8_t)word[i], (uint8_t)word[i + 1])) {
        clusters[count] += word.substring(i, i + 2);
        i += 2;
      }
      count++;
    } else {
      clusters[count++] = word.substring(i, i + 1);
      i++;
    }
  }
  String result;
  for (int j = count - 1; j >= 0; j--)
    result += clusters[j];
  return result;
}

// ──────────────────────────────────────────────────────
//  Draw a single line of Hebrew text centred at cx, top at y.
//  Each word is reversed for LTR rendering, and words are
//  placed right-to-left across the line.
// ──────────────────────────────────────────────────────
int drawHebrewLine(const String& text, int cx, int y, int scale) {
  if (text.length() == 0) return 0;

  const GFXfont* font = &NotoSerifHebrew_Bold_85;

  String words[10];
  int wordCount = 0;
  int start = 0;
  for (int i = 0; i <= (int)text.length(); i++) {
    if (i == (int)text.length() || text[i] == ' ') {
      if (i > start && wordCount < 10)
        words[wordCount++] = text.substring(start, i);
      start = i + 1;
    }
  }

  String reversed[10];
  for (int i = 0; i < wordCount; i++)
    reversed[i] = reverseHebrew(words[i]);

  int totalW = 0;
  for (int i = 0; i < wordCount; i++) {
    totalW += measureHebrewWord(font, reversed[i]);
    if (i < wordCount - 1) totalW += HEBREW_SPACE_W;
  }

  int curX = cx - totalW / 2;
  int fontH = FONT_BASE_H * scale;

  for (int i = wordCount - 1; i >= 0; i--) {
    int wordW = drawHebrewWord(font, reversed[i], curX, y);
    curX += wordW;
    if (i > 0) curX += HEBREW_SPACE_W;
  }

  return fontH;
}

// ──────────────────────────────────────────────────────
//  Draw lines vertically centred inside the TIME_BOX.
// ──────────────────────────────────────────────────────
void drawCenteredLines(const String& l1, const String& l2, const String& l3, int numLines) {
  int cx    = SCREEN_W / 2;
  int fontH = FONT_BASE_H * TEXT_SCALE;
  int visH  = FONT_ASCENT + (numLines - 1) * (fontH + LINE_GAP) + FONT_DESCENT;
  int y     = TIME_BOX_Y + (TIME_BOX_H - visH) / 2 + FONT_ASCENT;

  drawHebrewLine(l1, cx, y, TEXT_SCALE);
  drawHebrewLine(l2, cx, y + fontH + LINE_GAP, TEXT_SCALE);
  if (numLines == 3)
    drawHebrewLine(l3, cx, y + 2 * (fontH + LINE_GAP), TEXT_SCALE);
}

// ──────────────────────────────────────────────────────
//  Build the two-line phrase for the current time
// ──────────────────────────────────────────────────────
static int countWords(const String& s) {
  int n = 0;
  bool inWord = false;
  for (int i = 0; i < (int)s.length(); i++) {
    if (s[i] == ' ') { inWord = false; }
    else if (!inWord) { inWord = true; n++; }
  }
  return n;
}

void splitTimePhrase(const struct tm& t, String& line1, String& line2, String& line3) {
  int hour12 = t.tm_hour % 12;
  if (hour12 == 0) hour12 = 12;
  int min = t.tm_min;
  String period = String(getTimePeriod(t.tm_hour));
  line3 = "";

  if (isSubtractMinute(min)) {
    int next = (hour12 % 12) + 1;       // 12 -> 1
    line1 = String(SUBTRACT_AMOUNT[min]) + " " + String(HOURS_LAMED[next - 1]);
    line2 = period;
  } else if (min == 0) {
    line1 = String(HOURS[hour12 - 1]);
    line2 = period;
  } else {
    String minPart = String(MINUTE_PREFIX[min]);
    if (hour12 >= 11 || countWords(minPart) == 3) {
      line1 = String(HOURS[hour12 - 1]);
      line2 = minPart;
      line3 = period;
    } else {
      line1 = String(HOURS[hour12 - 1]) + " " + minPart;
      line2 = period;
    }
  }
}

// ──────────────────────────────────────────────────────
//  Main draw routine
//  fullRefresh=true on first boot only — clears the whole
//  screen and uses full e-ink update. Subsequent calls
//  redraw only the time box and use partial refresh.
// ──────────────────────────────────────────────────────
void drawTimeInWords(const struct tm& t, bool fullRefresh) {
  String line1, line2, line3;
  splitTimePhrase(t, line1, line2, line3);

  int numLines = (line3.length() > 0) ? 3 : 2;

  // Check if anything actually changed since last draw
  if (!fullRefresh) {
    bool anyChanged = (numLines != prevLineCount);
    if (!anyChanged) {
      for (int i = 0; i < numLines; i++) {
        const char* prev = prevLines[i];
        const char* cur  = (i == 0) ? line1.c_str() : (i == 1) ? line2.c_str() : line3.c_str();
        if (strcmp(cur, prev) != 0) {
          anyChanged = true;
          break;
        }
      }
    }
    if (!anyChanged) {
      Serial.println("No lines changed — skipping refresh");
      return;
    }
  }

  Serial.printf("Drawing time hour=%d min=%d fullRefresh=%d\n",
    t.tm_hour, t.tm_min, fullRefresh);

  bool doFullRefresh = fullRefresh || (partialCount >= FULL_REFRESH_EVERY);

  // For differential partial refresh: render old text first to build
  // the pixel-perfect old buffer the UC8179 needs for clean transitions.
  uint8_t* oldBuf = nullptr;
  if (!doFullRefresh && prevLineCount > 0) {
    epaper.fillRect(TIME_BOX_X, TIME_BOX_Y, TIME_BOX_W, TIME_BOX_H, TFT_WHITE);
    drawCenteredLines(String(prevLines[0]), String(prevLines[1]),
                      String(prevLines[2]), prevLineCount);
    oldBuf = epaper.capturePartialWindow(TIME_BOX_X, TIME_BOX_Y, TIME_BOX_W, TIME_BOX_H);
  }

  // Render new text
  if (doFullRefresh) epaper.fillScreen(TFT_WHITE);
  else               epaper.fillRect(TIME_BOX_X, TIME_BOX_Y, TIME_BOX_W, TIME_BOX_H, TFT_WHITE);

  drawCenteredLines(line1, line2, line3, numLines);

  if (doFullRefresh) {
    epaper.update();
    partialCount = 0;
  } else {
    epaper.updataPartial(TIME_BOX_X, TIME_BOX_Y, TIME_BOX_W, TIME_BOX_H, oldBuf);
    partialCount++;
  }
  epdRamValid = true;

  if (oldBuf) free(oldBuf);

  // Save current state for next wake
  for (int i = 0; i < MAX_LINES; i++) {
    if (i < numLines) {
      const char* src = (i == 0) ? line1.c_str() : (i == 1) ? line2.c_str() : line3.c_str();
      strncpy(prevLines[i], src, sizeof(prevLines[i]) - 1);
    } else {
      prevLines[i][0] = '\0';
    }
  }
  prevLineCount = numLines;

  Serial.printf("Drew \"%s\" / \"%s\" / \"%s\"\n", line1.c_str(), line2.c_str(), line3.c_str());
}

// ──────────────────────────────────────────────────────
//  Error screen
// ──────────────────────────────────────────────────────
void drawError(const String& msg) {
  epdRamValid = false;
  epaper.fillScreen(TFT_WHITE);
  epaper.setTextColor(TFT_BLACK);
  epaper.drawCentreString("Error:",      SCREEN_W/2, SCREEN_H/2 - 30, 4);
  epaper.drawCentreString(msg.c_str(),   SCREEN_W/2, SCREEN_H/2 + 10, 2);
  epaper.update();
}

// ──────────────────────────────────────────────────────
//  Deep sleep
// ──────────────────────────────────────────────────────
void goToSleep(int seconds) {
  WiFi.disconnect(true);
  WiFi.mode(WIFI_OFF);
  time(&savedEpoch);
  savedSleepSec = seconds;
  storeSleepSnapshot();
  logClockState("before deep sleep");
  Serial.printf("Sleeping %d seconds...\n", seconds);
  esp_sleep_enable_timer_wakeup((uint64_t)seconds * 1000000ULL);
  esp_deep_sleep_start();
}

#else
// Stubs when EPAPER_ENABLE isn't defined (mirrors the original sketch's behaviour).
void setup() {
  Serial.begin(115200);
  delay(500);
  Serial.println("EPAPER: NOT DEFINED");
}
void loop() {}
#endif
