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
#include "esp_sleep.h"
#include <Fonts/Custom/Heebo_Bold_100.h>
#include "time_words.h"
#include "secrets.h"

#ifdef EPAPER_ENABLE

EPaper epaper = EPaper();

// ── Wi-Fi (used briefly for NTP sync, ~once per day) ──
const char* ssid     = WIFI_SSID;
const char* password = WIFI_PASSWORD;

// ── NTP ───────────────────────────────────────────────
const char* ntpServer          = "pool.ntp.org";
const char* timeZone           = "UTC-6"; // POSIX TZ: "UTC-6" means UTC+6

// ── Sleep / refresh schedule ──────────────────────────
#define SLEEP_FAST_SEC   120
#define SLEEP_SLOW_SEC   300
#define WIFI_TIMEOUT     20

// ── Layout ────────────────────────────────────────────
#define SCREEN_W         800
#define SCREEN_H         480
#define TEXT_SCALE       1
#define FONT_BASE_H      100
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

// ──────────────────────────────────────────────────────
//  setup() — entry point on every wake
// ──────────────────────────────────────────────────────
void setup() {
  Serial.begin(115200);
  delay(300);

  bootCount++;
  Serial.printf("Boot #%d\n", bootCount);

  epaper.begin();
  epaper.setRotation(0);

  // Restore timezone on every boot without restarting SNTP.
  applyTimeZone();

  struct tm t;
  bool haveTime = getLocalTime(&t);
  bool needNTP  = firstBoot || !haveTime || (haveTime && t.tm_yday != ntpDay);

  if (needNTP) {
    for (int attempt = 0; attempt < 3; attempt++) {
      ntpSync();
      if (getLocalTime(&t)) break;
      delay(2000);
    }
    if (!getLocalTime(&t)) {
      drawError(String("NTP failed - retrying in ") + SLEEP_FAST_SEC + "s");
      goToSleep(SLEEP_FAST_SEC);
      return;
    }
    ntpDay = t.tm_yday;
  }

  drawTimeInWords(t, firstBoot);
  firstBoot = false;

  goToSleep(sleepSeconds(t));
}

void loop() {}

void applyTimeZone() {
  setenv("TZ", timeZone, 1);
  tzset();
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
  WiFi.begin(ssid, password);
  int tries = 0;
  while (WiFi.status() != WL_CONNECTED && tries < WIFI_TIMEOUT) { delay(500); tries++; }
  if (WiFi.status() == WL_CONNECTED) {
    configTzTime(timeZone, ntpServer);
    struct tm t;
    int ntpTries = 0;
    while (!getLocalTime(&t) && ntpTries < 10) { delay(1000); ntpTries++; }
  } else {
    Serial.println("WiFi failed — keeping previous time");
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

  // Split text into words (space-separated)
  int wordCount = 0;
  String words[8];
  int start = 0;
  for (int i = 0; i <= (int)text.length(); i++) {
    if (i == (int)text.length() || text[i] == ' ') {
      if (i > start && wordCount < 8) {
        words[wordCount++] = text.substring(start, i);
      }
      start = i + 1;
    }
  }

  epaper.setFreeFont(&Heebo_Bold_100);
  epaper.setTextColor(TFT_BLACK, TFT_WHITE);
  epaper.setTextSize(scale);

  // Measure total width
  int totalW = 0;
  for (int i = 0; i < wordCount; i++) {
    totalW += epaper.textWidth(words[i].c_str());
    if (i < wordCount - 1) totalW += HEBREW_SPACE_W;
  }

  // Hebrew RTL: first word in source goes rightmost,
  // so draw words in reverse order left-to-right.
  int curX = cx - totalW / 2;
  int fontH = FONT_BASE_H * scale;

  for (int i = wordCount - 1; i >= 0; i--) {
    epaper.drawString(words[i].c_str(), curX, y);
    curX += epaper.textWidth(words[i].c_str());
    if (i > 0) curX += HEBREW_SPACE_W;
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
  epaper.setTextColor(TFT_BLACK, TFT_WHITE);
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
