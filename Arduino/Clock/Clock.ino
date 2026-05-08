// Smart Board — Hebrew word clock
//
// Displays the current time in Hebrew words with niqud (vowel marks)
// on a 7.5" e-ink display (Seeed XIAO e-paper driver, model 502).
//
// Update cadence:
//   - Every 5 minutes during quiet hours
//       (Sun-Thu 09:00-13:00, all days 00:00-06:00)
//   - Every 2 minutes otherwise
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
#include <stdlib.h>
#include "esp_sleep.h"
#include <Fonts/Custom/Heebo_Bold_85.h>
#include "time_words.h"
#include "secrets.h"

#ifdef EPAPER_ENABLE

EPaper epaper = EPaper();

// ── Wi-Fi (used briefly for NTP sync, ~once per day) ──
const char* ssid     = WIFI_SSID;
const char* password = WIFI_PASSWORD;

// ── NTP ───────────────────────────────────────────────
const char* ntpServer          = "pool.ntp.org";
const char* timeZone           = "IST-2IDT,M3.4.4/26,M10.5.0"; // Israel: UTC+2 standard, UTC+3 during DST
const bool  ENABLE_TIME_DEBUG  = true;

// ── Sleep / refresh schedule ──────────────────────────
#define SLEEP_FAST_SEC   120
#define SLEEP_SLOW_SEC   300
#define WIFI_TIMEOUT     20

// ── Layout ────────────────────────────────────────────
#define SCREEN_W         800
#define SCREEN_H         480
#define TEXT_SCALE       1
#define FONT_BASE_H      85
#define HEBREW_SPACE_W   20
#define LINE_GAP         24

// Rectangle that gets cleared & redrawn (must satisfy 8-px X alignment for partial refresh)
#define TIME_BOX_X       0
#define TIME_BOX_Y       40
#define TIME_BOX_W       800
#define TIME_BOX_H       400

// ── State preserved across deep-sleep cycles ──────────
RTC_DATA_ATTR int  ntpDay    = -1;
RTC_DATA_ATTR int  bootCount = 0;
RTC_DATA_ATTR bool firstBoot = true;

struct TimeDebugSnapshot {
  time_t epoch;
  int    localYday;
  int    localHour;
  int    localMin;
  int    utcYday;
  int    utcHour;
  int    utcMin;
  int    ntpDay;
  int    bootCount;
  bool   firstBoot;
};

RTC_DATA_ATTR TimeDebugSnapshot lastSleepSnapshot;
RTC_DATA_ATTR bool              lastSleepSnapshotValid = false;

// ── Types ─────────────────────────────────────────────
struct HebrewToken {
  String letter;        // one base letter, OR " " for a space
  String marks[4];      // up to 4 niqud per base letter
  int    markCount;
  bool   isSpace;
};

// ── Forward declarations ──────────────────────────────
void   applyTimeZone();
void   ntpSync();
void   drawTimeInWords(const struct tm& t, bool fullRefresh);
void   drawError(const String& msg);
void   goToSleep(int seconds);
bool   isLowFrequencyTime(const struct tm& t);
int    sleepSeconds(const struct tm& t);
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
  Serial.printf(
    "Boot #%d wakeCause=%s(%d) firstBoot=%d ntpDay=%d\n",
    bootCount,
    wakeCauseName(esp_sleep_get_wakeup_cause()),
    (int)esp_sleep_get_wakeup_cause(),
    firstBoot,
    ntpDay
  );
  printSleepSnapshot();
  logClockState("boot/before applyTimeZone");

  epaper.begin();
  epaper.setRotation(0);

  // Restore timezone on every boot without restarting SNTP.
  applyTimeZone();
  logClockState("boot/after applyTimeZone");

  struct tm t;
  bool haveTime = getLocalTime(&t);
  logGetLocalTimeResult("boot/initial", haveTime, haveTime ? &t : nullptr);
  bool needNTP  = firstBoot || !haveTime || (haveTime && t.tm_yday != ntpDay);
  Serial.printf(
    "needNTP=%d (firstBoot=%d haveTime=%d tm_yday=%d ntpDay=%d)\n",
    needNTP,
    firstBoot,
    haveTime,
    haveTime ? t.tm_yday : -1,
    ntpDay
  );

  if (needNTP) {
    for (int attempt = 0; attempt < 3; attempt++) {
      Serial.printf("NTP attempt %d/3\n", attempt + 1);
      ntpSync();
      bool synced = getLocalTime(&t);
      logGetLocalTimeResult("boot/post-ntp", synced, synced ? &t : nullptr);
      logClockState("boot/post-ntp");
      if (synced) break;
      delay(2000);
    }
    if (!getLocalTime(&t)) {
      logClockState("boot/ntp-failed");
      drawError(String("NTP failed - retrying in ") + SLEEP_FAST_SEC + "s");
      goToSleep(SLEEP_FAST_SEC);
      return;
    }
    ntpDay = t.tm_yday;
    Serial.printf("Updated ntpDay=%d\n", ntpDay);
  }

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
  lastSleepSnapshot.bootCount = bootCount;
  lastSleepSnapshot.firstBoot = firstBoot;
  lastSleepSnapshotValid      = true;
}

void printSleepSnapshot() {
  if (!ENABLE_TIME_DEBUG || !lastSleepSnapshotValid) return;

  Serial.printf(
    "Prev sleep snapshot: boot=%d epoch=%lld local=yday %d %02d:%02d utc=yday %d %02d:%02d ntpDay=%d firstBoot=%d\n",
    lastSleepSnapshot.bootCount,
    (long long)lastSleepSnapshot.epoch,
    lastSleepSnapshot.localYday,
    lastSleepSnapshot.localHour,
    lastSleepSnapshot.localMin,
    lastSleepSnapshot.utcYday,
    lastSleepSnapshot.utcHour,
    lastSleepSnapshot.utcMin,
    lastSleepSnapshot.ntpDay,
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

int sleepSeconds(const struct tm& t) {
  return isLowFrequencyTime(t) ? SLEEP_SLOW_SEC : SLEEP_FAST_SEC;
}

// ──────────────────────────────────────────────────────
//  NTP sync — turn WiFi on briefly, sync, turn off
// ──────────────────────────────────────────────────────
void ntpSync() {
  Serial.println("NTP sync...");
  logClockState("ntpSync/before WiFi");
  WiFi.begin(ssid, password);
  int tries = 0;
  while (WiFi.status() != WL_CONNECTED && tries < WIFI_TIMEOUT) { delay(500); tries++; }
  Serial.printf("WiFi status=%d after %d waits\n", WiFi.status(), tries);
  if (WiFi.status() == WL_CONNECTED) {
    Serial.printf("Calling configTzTime(TZ=%s, server=%s)\n", timeZone, ntpServer);
    configTzTime(timeZone, ntpServer);
    logClockState("ntpSync/after configTzTime");
    struct tm t;
    bool gotTime = getLocalTime(&t);
    int ntpTries = 0;
    while (!gotTime && ntpTries < 10) {
      ntpTries++;
      Serial.printf("Waiting for NTP time... %d/10\n", ntpTries);
      delay(1000);
      gotTime = getLocalTime(&t);
    }
    logGetLocalTimeResult("ntpSync/final", gotTime, gotTime ? &t : nullptr);
    logClockState("ntpSync/final");
  } else {
    Serial.println("WiFi failed — keeping previous time");
  }
  if (WiFi.status() != WL_CONNECTED) {
    logClockState("ntpSync/WiFi-failed");
  }
  WiFi.disconnect(true);
  WiFi.mode(WIFI_OFF);
}

// ──────────────────────────────────────────────────────
//  Hebrew tokenizer — groups each base letter with the
//  niqud (combining marks) that follow it in source order.
// ──────────────────────────────────────────────────────
bool isHebrewNikudUtf8(uint8_t b0, uint8_t b1) {
  // U+05B0..U+05BD  → 0xD6 0xB0..0xBD  (sheva, hatafs, vowels, dagesh, meteg)
  // U+05BF          → 0xD6 0xBF        (rafe)
  // U+05C1, U+05C2  → 0xD7 0x81, 0x82  (shin/sin dot)
  // U+05C7          → 0xD7 0x87        (qamatz qatan)
  if (b0 == 0xD6 && b1 >= 0xB0 && b1 <= 0xBD) return true;
  if (b0 == 0xD6 && b1 == 0xBF) return true;
  if (b0 == 0xD7 && (b1 == 0x81 || b1 == 0x82 || b1 == 0x87)) return true;
  return false;
}

int tokenizeHebrew(const String& s, HebrewToken* out, int maxTokens) {
  int count = 0, i = 0;
  while (i < (int)s.length() && count < maxTokens) {
    char c = s[i];
    if (c == ' ') {
      out[count].letter    = " ";
      out[count].markCount = 0;
      out[count].isSpace   = true;
      count++; i++;
    } else if (((uint8_t)c & 0xE0) == 0xC0) {
      if (i + 1 >= (int)s.length()) { i++; continue; }
      out[count].letter    = s.substring(i, i + 2);
      out[count].markCount = 0;
      out[count].isSpace   = false;
      i += 2;
      while (i + 1 < (int)s.length() &&
             isHebrewNikudUtf8((uint8_t)s[i], (uint8_t)s[i+1]) &&
             out[count].markCount < 4) {
        out[count].marks[out[count].markCount++] = s.substring(i, i + 2);
        i += 2;
      }
      count++;
    } else {
      out[count].letter    = s.substring(i, i + 1);
      out[count].markCount = 0;
      out[count].isSpace   = false;
      count++; i++;
    }
  }
  return count;
}

// ──────────────────────────────────────────────────────
//  Draw a niqud mark as primitive shapes.
//  cx     = horizontal centre of the base letter
//  yTop   = top y of the letter
//  fontH  = visual letter height at current scale
//  scale  = textSize multiplier (so niqud scale together)
//
//  NOTE: visual offsets here are first-pass guesses.
//  Tweak by eye after seeing real output.
// ──────────────────────────────────────────────────────
void drawNikudMark(const String& mark, int cx, int yTop, int fontH, int scale) {
  if (mark.length() < 2) return;
  uint8_t b0 = (uint8_t)mark[0], b1 = (uint8_t)mark[1];
  uint16_t cp = ((b0 & 0x1F) << 6) | (b1 & 0x3F);

  int baseline = yTop + fontH;
  int dotR     = max(2, fontH / 20);
  int gap      = fontH / 8;

  int sp = dotR * 3;  // spacing between dots
  int lineW = fontH / 4;
  int lineH = max(2, fontH / 30);

  switch (cp) {
    case 0x05B0:  // sheva — two vertical dots below
      epaper.fillCircle(cx, baseline + gap,        dotR, TFT_BLACK);
      epaper.fillCircle(cx, baseline + gap + sp,   dotR, TFT_BLACK);
      break;
    case 0x05B1:  // hataf segol = sheva + segol
      epaper.fillCircle(cx + sp, baseline + gap,        dotR, TFT_BLACK);
      epaper.fillCircle(cx + sp, baseline + gap + sp,   dotR, TFT_BLACK);
      epaper.fillCircle(cx - sp/2, baseline + gap,      dotR, TFT_BLACK);
      epaper.fillCircle(cx - sp*2, baseline + gap,      dotR, TFT_BLACK);
      epaper.fillCircle(cx - sp,   baseline + gap + sp, dotR, TFT_BLACK);
      break;
    case 0x05B2:  // hataf patah = sheva + patah line
      epaper.fillCircle(cx + sp, baseline + gap,        dotR, TFT_BLACK);
      epaper.fillCircle(cx + sp, baseline + gap + sp,   dotR, TFT_BLACK);
      epaper.fillRect(cx - lineW - sp, baseline + gap + sp/2, lineW, lineH, TFT_BLACK);
      break;
    case 0x05B3:  // hataf qamatz = sheva + qamatz
      epaper.fillCircle(cx + sp, baseline + gap,        dotR, TFT_BLACK);
      epaper.fillCircle(cx + sp, baseline + gap + sp,   dotR, TFT_BLACK);
      epaper.fillRect(cx - lineW - sp, baseline + gap + sp/2,     lineW, lineH,  TFT_BLACK);
      epaper.fillRect(cx - sp - lineW/2, baseline + gap + sp/2,   lineH, sp,     TFT_BLACK);
      break;
    case 0x05B4:  // hiriq — single dot below
      epaper.fillCircle(cx, baseline + gap + sp/2, dotR, TFT_BLACK);
      break;
    case 0x05B5:  // tsere — two horizontal dots below
      epaper.fillCircle(cx - sp, baseline + gap + sp/2, dotR, TFT_BLACK);
      epaper.fillCircle(cx + sp, baseline + gap + sp/2, dotR, TFT_BLACK);
      break;
    case 0x05B6:  // segol — three dots in inverted triangle below
      epaper.fillCircle(cx - sp, baseline + gap,        dotR, TFT_BLACK);
      epaper.fillCircle(cx + sp, baseline + gap,        dotR, TFT_BLACK);
      epaper.fillCircle(cx,      baseline + gap + sp,   dotR, TFT_BLACK);
      break;
    case 0x05B7:  // patah — horizontal line below
      epaper.fillRect(cx - lineW/2, baseline + gap + sp/2, lineW, lineH, TFT_BLACK);
      break;
    case 0x05B8:  // qamatz — T-shape below (line + small vertical)
    case 0x05C7:  // qamatz qatan
      epaper.fillRect(cx - lineW/2, baseline + gap + sp/2,        lineW, lineH, TFT_BLACK);
      epaper.fillRect(cx - lineH/2, baseline + gap + sp/2 + lineH, lineH, sp,  TFT_BLACK);
      break;
    case 0x05B9:  // holam — dot above
    case 0x05BA:  // holam haser
      epaper.fillCircle(cx, yTop - gap, dotR, TFT_BLACK);
      break;
    case 0x05BB:  // qubuts — three diagonal dots below
      epaper.fillCircle(cx - sp, baseline + gap,          dotR, TFT_BLACK);
      epaper.fillCircle(cx,      baseline + gap + sp/2,   dotR, TFT_BLACK);
      epaper.fillCircle(cx + sp, baseline + gap + sp,     dotR, TFT_BLACK);
      break;
    case 0x05BC:  // dagesh / mappiq — dot inside letter
      epaper.fillCircle(cx, yTop + fontH/2, dotR, TFT_BLACK);
      break;
    case 0x05C1:  // shin dot — above-right
      epaper.fillCircle(cx + sp*2, yTop - gap, dotR, TFT_BLACK);
      break;
    case 0x05C2:  // sin dot — above-left
      epaper.fillCircle(cx - sp*2, yTop - gap, dotR, TFT_BLACK);
      break;
    default: break;
  }
}

// ──────────────────────────────────────────────────────
//  Draw a single line of Hebrew text (with niqud) centred at cx, top at y.
// ──────────────────────────────────────────────────────
int drawHebrewLine(const String& text, int cx, int y, int scale) {
  if (text.length() == 0) return 0;
  HebrewToken tokens[64];
  int n = tokenizeHebrew(text, tokens, 64);

  epaper.setFreeFont(&Heebo_Bold_85);
  epaper.setTextColor(TFT_BLACK);
  epaper.setTextSize(scale);

  int letterGap = 5;
  int totalW = 0;
  int letterCount = 0;
  for (int i = 0; i < n; i++) {
    if (tokens[i].isSpace) totalW += HEBREW_SPACE_W;
    else { totalW += epaper.textWidth(tokens[i].letter.c_str()); letterCount++; }
  }
  totalW += (letterCount - 1) * letterGap;

  int curX  = cx - totalW / 2;
  int fontH = FONT_BASE_H * scale;

  // Hebrew RTL: source[0] is rightmost on screen,
  // iterate in reverse while drawing left-to-right.
  for (int i = n - 1; i >= 0; i--) {
    HebrewToken& t = tokens[i];
    if (t.isSpace) { curX += HEBREW_SPACE_W; continue; }
    epaper.drawString(t.letter.c_str(), curX, y);
    curX += epaper.textWidth(t.letter.c_str()) + letterGap;
  }

  epaper.setTextSize(1);
  epaper.setTextFont(0);
  return fontH;
}

// ──────────────────────────────────────────────────────
//  Build the two-line phrase for the current time
// ──────────────────────────────────────────────────────
void splitTimePhrase(const struct tm& t, String& line1, String& line2) {
  int hour12 = t.tm_hour % 12;
  if (hour12 == 0) hour12 = 12;
  int min = t.tm_min;

  if (isSubtractMinute(min)) {
    int next = (hour12 % 12) + 1;       // 12 -> 1
    line1 = String(SUBTRACT_AMOUNT[min]);
    line2 = String(HOURS_LAMED[next - 1]);
  } else if (min == 0) {
    line1 = String(HOURS[hour12 - 1]);
    line2 = "";
  } else {
    line1 = String(HOURS[hour12 - 1]);
    line2 = String(MINUTE_PREFIX[min]);
  }
}

// ──────────────────────────────────────────────────────
//  Main draw routine
//  fullRefresh=true on first boot only — clears the whole
//  screen and uses full e-ink update. Subsequent calls
//  redraw only the time box and use partial refresh.
// ──────────────────────────────────────────────────────
void drawTimeInWords(const struct tm& t, bool fullRefresh) {
  String line1, line2;
  splitTimePhrase(t, line1, line2);
  Serial.printf(
    "Drawing time hour=%d min=%d fullRefresh=%d\n",
    t.tm_hour,
    t.tm_min,
    fullRefresh
  );

  if (fullRefresh) epaper.fillScreen(TFT_WHITE);
  else             epaper.fillRect(TIME_BOX_X, TIME_BOX_Y, TIME_BOX_W, TIME_BOX_H, TFT_WHITE);

  int cx     = SCREEN_W / 2;
  int fontH  = FONT_BASE_H * TEXT_SCALE;
  bool two   = line2.length() > 0;
  int totalH = (two ? 2 : 1) * fontH + (two ? LINE_GAP : 0);
  int y      = TIME_BOX_Y + (TIME_BOX_H - totalH) / 2;

  drawHebrewLine(line1, cx, y, TEXT_SCALE);
  if (two) drawHebrewLine(line2, cx, y + fontH + LINE_GAP, TEXT_SCALE);

  if (fullRefresh) {
    epaper.update();
  } else {
    epaper.updataPartial(TIME_BOX_X, TIME_BOX_Y, TIME_BOX_W, TIME_BOX_H);
  }
  Serial.printf("Drew \"%s\" / \"%s\"\n", line1.c_str(), line2.c_str());
}

// ──────────────────────────────────────────────────────
//  Error screen
// ──────────────────────────────────────────────────────
void drawError(const String& msg) {
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
