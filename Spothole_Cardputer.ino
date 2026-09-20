/*
  POTA + SOTA Spot Viewer for M5Stack Cardputer / Cardputer-Adv
  --------------------------------------------------------------
  Fetches live spots from both Parks on the Air and Summits on the Air,
  merges them into a single time-sorted list, and displays on the 240x135
  screen.

  Sources:
    POTA  https://api.pota.app/spot/activator         (all current spots)
    SOTA  https://api2.sota.org.uk/api/spots/-1/all   (last 1 hour)

  Each spot takes two display rows:
    Line 1: P/S  HH:MM  CALLSIGN  FREQ   MODE  AGEm
                 P = POTA (green), S = SOTA (yellow)
    Line 2: REFERENCE / SUMMIT CODE  Full name  (cyan)

  WiFi credentials are entered on first boot via the keyboard and saved to
  NVS flash (ESP32 Preferences) — survive power cycles.

  Libraries needed (Arduino IDE -> Library Manager):
    - M5Cardputer   (https://github.com/m5stack/M5Cardputer)
                    -> accept "Install All" for M5Unified / M5GFX
    - ArduinoJson   (by Benoit Blanchon, v6.x)
  WiFi / WiFiClientSecure / HTTPClient / Preferences / time all ship
  with the ESP32 Arduino core — no separate install needed.

  Board: Tools > Board > "M5Cardputer"

  Controls:
    ;   scroll up one spot
    .   scroll down one spot
    ,   page up
    /   page down
    r   refresh both feeds now
    f   toggle VK/ZL-only filter
    w   re-enter WiFi credentials
*/

#include "M5Cardputer.h"
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <Preferences.h>
#include <time.h>
#include "esp_log.h"

String truncStr(const String &s, int maxLen);   // forward declaration

// ---- API endpoint ----
const char* SPOTHOLE_URL = "https://spothole.app/api/v1/spots?limit=50";

// ---- tuneable constants ----
const unsigned long AUTO_REFRESH_MS = 5UL * 60UL * 1000UL;
const int MAX_SPOTS     = 50;   // total spots stored
const int MAX_INPUT_LEN = 63;
const int ROW_H         = 30;   // px per spot (three 10-px text lines)
const int LINE_CHARS    = 40;   // chars at textSize(1) on 240px

// ---- WiFi credentials ----
char wifiSSID[MAX_INPUT_LEN + 1]     = "";
char wifiPassword[MAX_INPUT_LEN + 1] = "";

Preferences prefs;

// ---- Unified spot record ----
struct Spot {
  char   source;    // 'P' = POTA, 'S' = SOTA
  String time;      // "HH:MM" UTC
  time_t epoch;     // for age + sort
  String callsign;
  String freq;      // MHz as string
  String mode;
  String reference; // park ref or summit code
  String name;      // park name or summit details
  String region;    // locationDesc (POTA) or associationCode (SOTA)
  String comments;
};

Spot spots[MAX_SPOTS];
int  spotCount    = 0;
int  scrollOffset = 0;
int  rowsVisible  = 1;

// ---- VK/ZL filter ----
bool filterVKZL      = false;

// ---- Mode filter (cycles on 'm') ----
const char* MODE_LIST[] = { "ALL", "SSB", "CW", "FT8", "FM" };
const int   MODE_COUNT  = 5;
int         modeFilterIdx = 0;   // 0 = ALL (no filter)

// ---- Sort mode ----
bool sortByBand = false;   // false = newest first, true = ascending frequency

int  visibleIdx[MAX_SPOTS];
int  visibleCount    = 0;

void rebuildVisible() {
  visibleCount = 0;
  for (int i = 0; i < spotCount; i++) {

    // --- Drop spots older than 60 minutes (once NTP is synced) ---
    int age = spotAgeMinutes(spots[i].epoch);
    if (age > 60) continue;

    // --- VK/ZL filter ---
    if (filterVKZL) {
      //String ref = spots[i].reference;
      String ref = spots[i].callsign;
      ref.toUpperCase();
      if (!ref.startsWith("VK") && !ref.startsWith("ZL")) continue;
    }

    // --- Mode filter ---
    if (modeFilterIdx > 0) {
      String m = spots[i].mode;
      m.toUpperCase();
      if (m != MODE_LIST[modeFilterIdx]) continue;
    }

    visibleIdx[visibleCount++] = i;
  }
  scrollOffset = 0;
}

// Sort spots[] by epoch (newest first) or by frequency (ascending = band order)
void sortSpots() {
  for (int i = 1; i < spotCount; i++) {
    Spot key = spots[i];
    int j = i - 1;
    if (sortByBand) {
      float keyFreq = key.freq.toFloat();
      while (j >= 0 && spots[j].freq.toFloat() > keyFreq) {
        spots[j + 1] = spots[j];
        j--;
      }
    } else {
      while (j >= 0 && spots[j].epoch < key.epoch) {
        spots[j + 1] = spots[j];
        j--;
      }
    }
    spots[j + 1] = key;
  }
}

unsigned long lastFetchMs = 0;
String        statusLine  = "Booting...";

// ==================================================================
//  TIME HELPERS
// ==================================================================

// Parse "YYYY-MM-DDTHH:MM:SS" (UTC) → time_t
// Works because configTime(0,0,...) keeps ESP32 in UTC.
time_t parseISOTime(const String &s) {
  struct tm t = {};
  if (sscanf(s.c_str(), "%d-%d-%dT%d:%d:%d",
             &t.tm_year, &t.tm_mon, &t.tm_mday,
             &t.tm_hour, &t.tm_min, &t.tm_sec) != 6) return 0;
  t.tm_year -= 1900;
  t.tm_mon  -= 1;
  t.tm_isdst = 0;
  return mktime(&t);
}

// Age in whole minutes; -1 if NTP not synced yet
int spotAgeMinutes(time_t epoch) {
  time_t now = time(nullptr);
  if (now < 1000000 || epoch == 0) return -1;
  return (int)max(0L, (long)(now - epoch) / 60L);
}

// ==================================================================
//  DISPLAY HELPERS
// ==================================================================

void drawHeader() {
  M5Cardputer.Display.fillRect(0, 0, M5Cardputer.Display.width(), 12, TFT_NAVY);
  M5Cardputer.Display.setTextColor(TFT_WHITE, TFT_NAVY);
  M5Cardputer.Display.setCursor(2, 2);

  char utcBuf[9] = "--:--z";
  struct tm ti;
  if (getLocalTime(&ti, 0))
    snprintf(utcBuf, sizeof(utcBuf), "%02d:%02dz", ti.tm_hour, ti.tm_min);

  const char* vkTag   = filterVKZL       ? " VK/ZL" : "";
  const char* modeTag = (modeFilterIdx > 0) ? MODE_LIST[modeFilterIdx] : "";
  const char* bandTag = sortByBand       ? "BAND"   : "";
  // separators between active tags
  const char* sep1    = (filterVKZL && modeFilterIdx > 0)               ? "/" : "";
  const char* sep2    = ((filterVKZL || modeFilterIdx > 0) && sortByBand) ? "/" : "";
  char tags[20];
  snprintf(tags, sizeof(tags), "%s%s%s%s%s", vkTag, sep1, modeTag, sep2, bandTag);

  char header[48];
  snprintf(header, sizeof(header), "Spothole %s(%d)%-*s%s",
           tags,
           visibleCount,
           (int)(42 - 14 - (int)strlen(tags) - (int)String(visibleCount).length() - (int)strlen(utcBuf)), "",
           utcBuf);
  M5Cardputer.Display.print(header);
}

void drawFooter() {
  int y = M5Cardputer.Display.height() - 10;
  M5Cardputer.Display.fillRect(0, y, M5Cardputer.Display.width(), 10, TFT_DARKGREY);
  M5Cardputer.Display.setTextColor(TFT_WHITE, TFT_DARKGREY);

  // Position counter on the left: which spot is at the top, e.g. "3/34"
  char posBuf[12];
  if (visibleCount == 0) snprintf(posBuf, sizeof(posBuf), "0/0");
  else snprintf(posBuf, sizeof(posBuf), "%d/%d", scrollOffset + 1, visibleCount);

  M5Cardputer.Display.setCursor(2, y + 1);
  M5Cardputer.Display.print(posBuf);

  // Status text follows, shifted right by FOOTER_GAP characters
  const int FOOTER_GAP = 6;   // 1 original space + 5 extra
  int statusMax = LINE_CHARS - (int)strlen(posBuf) - FOOTER_GAP;
  for (int i = 0; i < FOOTER_GAP; i++) M5Cardputer.Display.print(" ");
  M5Cardputer.Display.print(truncStr(statusLine, statusMax));
}

String truncStr(const String &s, int maxLen) {
  if ((int)s.length() <= maxLen) return s;
  return s.substring(0, maxLen - 1) + ">";
}

// Derive amateur band name from frequency in MHz
String freqToBand(float mhz) {
  if (mhz >= 1.8   && mhz < 2.0)    return "160m";
  if (mhz >= 3.5   && mhz < 4.0)    return "80m";
  if (mhz >= 5.3   && mhz < 5.4)    return "60m";
  if (mhz >= 7.0   && mhz < 7.3)    return "40m";
  if (mhz >= 10.1  && mhz < 10.2)   return "30m";
  if (mhz >= 14.0  && mhz < 14.35)  return "20m";
  if (mhz >= 18.0  && mhz < 18.2)   return "17m";
  if (mhz >= 21.0  && mhz < 21.45)  return "15m";
  if (mhz >= 24.89 && mhz < 25.0)   return "12m";
  if (mhz >= 28.0  && mhz < 29.7)   return "10m";
  if (mhz >= 50.0  && mhz < 54.0)   return "6m";
  if (mhz >= 144.0 && mhz < 148.0)  return "2m";
  if (mhz >= 430.0 && mhz < 440.0)  return "70cm";
  return "?";
}
// other digital (RTTY/PSK/DATA)=orange, unknown=light grey
// Colour per band
uint16_t bandColour(const String &band) {
  if (band == "160m") return TFT_RED;
  if (band == "80m")  return TFT_ORANGE;
  if (band == "60m")  return TFT_YELLOW;
  if (band == "40m")  return TFT_GREEN;
  if (band == "30m")  return TFT_BLUE;
  if (band == "20m")  return TFT_PINK;
  if (band == "17m")  return TFT_CYAN;
  if (band == "15m")  return TFT_MAGENTA;
  if (band == "12m")  return TFT_LIGHTGREY;
  if (band == "10m")  return TFT_WHITE;
  if (band == "6m")   return TFT_PURPLE;
  if (band == "2m")   return TFT_MAROON;
  if (band == "70cm") return TFT_OLIVE;
  return TFT_DARKGREY;
}

uint16_t modeColour(const String &mode) {
  String m = mode;
  m.toUpperCase();
  if (m == "CW")                              return TFT_GREEN;
  if (m == "SSB" || m == "LSB" || m == "USB"
   || m == "AM"  || m == "PHONE")             return TFT_WHITE;
  if (m == "FT8" || m == "FT4")              return TFT_MAGENTA;
  if (m == "FM")                              return TFT_YELLOW;
  if (m == "RTTY" || m == "PSK" || m == "DATA"
   || m == "DIGITAL" || m == "JS8")          return TFT_ORANGE;
  return TFT_LIGHTGREY;
}

void drawList() {
  const int top    = 13;
  const int bottom = M5Cardputer.Display.height() - 11;
  rowsVisible = (bottom - top) / ROW_H;
  if (rowsVisible < 1) rowsVisible = 1;

  M5Cardputer.Display.fillRect(0, top, M5Cardputer.Display.width(),
                                bottom - top, TFT_BLACK);

  if (visibleCount == 0) {
    M5Cardputer.Display.setTextColor(TFT_WHITE, TFT_BLACK);
    M5Cardputer.Display.setCursor(2, top + 2);
    M5Cardputer.Display.print(spotCount == 0 ? "No spots loaded."
                                              : "No VK/ZL spots.");
    return;
  }

  int maxOff = max(0, visibleCount - rowsVisible);
  scrollOffset = constrain(scrollOffset, 0, maxOff);

  for (int i = 0; i < rowsVisible; i++) {
    int vi = scrollOffset + i;
    if (vi >= visibleCount) break;
    Spot &s = spots[visibleIdx[vi]];

    int y1 = top + i * ROW_H;
    int y2 = y1 + 10;
    int y3 = y1 + 20;

    if (i > 0)
      M5Cardputer.Display.drawFastHLine(0, y1 - 1,
                                        M5Cardputer.Display.width(), TFT_DARKGREY);

    // --- Line 1: P/S  HH:MM  CALLSIGN  FREQ   MODE  AGEm ---
    char ageBuf[6];
    int  age = spotAgeMinutes(s.epoch);
    if      (age < 0)   snprintf(ageBuf, sizeof(ageBuf), "--m");
    else if (age < 100) snprintf(ageBuf, sizeof(ageBuf), "%dm", age);
    else                snprintf(ageBuf, sizeof(ageBuf), "%dh", age / 60);

    char line1[LINE_CHARS + 2];
    // Layout: time(5)+sp(1)+call(9) = 15 | freq(7)+sp(1)+band(4) = 12 | sp(1)+mode(4) = 5 | age(6) = 38
    String band = freqToBand(s.freq.toFloat());
    snprintf(line1, sizeof(line1), "%s %-9s%-7s %-4s %-7s%4s", //-4s%4s
             s.time.c_str(),
             truncStr(s.callsign, 9).c_str(),
             s.freq.c_str(),
             band.c_str(),
             s.mode.c_str(),
             ageBuf);

    // Colour by programme: POTA=cyan, WWFF=green, SOTA=yellow, others=white
    String sig = s.region;
    sig.toUpperCase();
    uint16_t col1;
    if      (sig == "SOTA") col1 = TFT_YELLOW;
    else if (sig == "POTA") col1 = TFT_CYAN;
    else if (sig == "WWFF") col1 = TFT_GREEN;
    else                    col1 = TFT_WHITE;

    // Age colour: red <5m, orange 5-15m, cyan >15m, grey if NTP not synced
    uint16_t ageCol;
    if      (age < 0)  ageCol = TFT_DARKGREY;
    else if (age < 5)  ageCol = TFT_RED;
    else if (age < 15) ageCol = TFT_ORANGE;
    else               ageCol = TFT_CYAN;

    uint16_t bCol = bandColour(band);

    // Colour segments:
    //   0-14  (time+sp+callsign)   → source colour
    //   15-26 (freq+sp+band)       → band colour
    //   27-31 (sp+mode)            → mode colour
    //   32+   (age)                → age colour
    M5Cardputer.Display.setTextColor(col1, TFT_BLACK);
    M5Cardputer.Display.setCursor(2, y1);
    M5Cardputer.Display.print(String(line1).substring(0, 15));

    M5Cardputer.Display.setTextColor(bCol, TFT_BLACK);
    M5Cardputer.Display.print(String(line1).substring(15, 27));

    M5Cardputer.Display.setTextColor(modeColour(s.mode), TFT_BLACK);
    M5Cardputer.Display.print(String(line1).substring(27, 32));

    M5Cardputer.Display.setTextColor(ageCol, TFT_BLACK);
    M5Cardputer.Display.print(String(line1).substring(32));

    // --- Line 2: REFERENCE / SUMMIT  Full name ---
    String locLine = s.reference + "  " + s.name;
    M5Cardputer.Display.setTextColor(TFT_DARKCYAN, TFT_BLACK);
    M5Cardputer.Display.setCursor(2, y2);
    M5Cardputer.Display.print(truncStr(locLine, LINE_CHARS));

    // --- Line 3: Comments ---
    M5Cardputer.Display.setTextColor(TFT_DARKGREY, TFT_BLACK);
    M5Cardputer.Display.setCursor(2, y3);
    M5Cardputer.Display.print(truncStr(s.comments, LINE_CHARS));
  }
}

void redraw() {
  drawHeader();
  drawList();
  drawFooter();
}

// ==================================================================
//  WIFI CREDENTIAL INPUT
// ==================================================================

String promptInput(const String &label, bool hidden) {
  const int DY = M5Cardputer.Display.height();
  const int DX = M5Cardputer.Display.width();

  M5Cardputer.Display.fillScreen(TFT_BLACK);
  M5Cardputer.Display.fillRect(0, 0, DX, 12, TFT_NAVY);
  M5Cardputer.Display.setTextColor(TFT_WHITE, TFT_NAVY);
  M5Cardputer.Display.setCursor(2, 2);
  M5Cardputer.Display.print("WiFi Setup");

  M5Cardputer.Display.setTextColor(TFT_YELLOW, TFT_BLACK);
  M5Cardputer.Display.setCursor(2, 18);
  M5Cardputer.Display.print(label);

  M5Cardputer.Display.setTextColor(TFT_DARKGREY, TFT_BLACK);
  M5Cardputer.Display.setCursor(2, DY - 20);
  M5Cardputer.Display.print("ENTER=ok  DEL=bksp");

  String input = "";
  auto redrawInput = [&]() {
    M5Cardputer.Display.fillRect(0, 30, DX, 30, TFT_BLACK);
    M5Cardputer.Display.setTextColor(TFT_GREEN, TFT_BLACK);
    M5Cardputer.Display.setCursor(2, 32);
    if (hidden) {
      String stars = "";
      for (size_t i = 0; i < input.length(); i++) stars += '*';
      M5Cardputer.Display.print(stars + "_");
    } else {
      M5Cardputer.Display.print(input + "_");
    }
  };
  redrawInput();

  while (true) {
    M5Cardputer.update();
    if (M5Cardputer.Keyboard.isChange() && M5Cardputer.Keyboard.isPressed()) {
      Keyboard_Class::KeysState st = M5Cardputer.Keyboard.keysState();
      if (st.enter) return input;
      if (st.del && input.length() > 0) { input.remove(input.length() - 1); redrawInput(); }
      for (char c : st.word) {
        if (c >= 0x20 && (int)input.length() < MAX_INPUT_LEN) { input += c; redrawInput(); }
      }
    }
    delay(5);
  }
}

// ==================================================================
//  CREDENTIALS: LOAD / SAVE / PROMPT
// ==================================================================

void loadCredentials() {
  prefs.begin("wifi", true);
  String s = prefs.getString("ssid", "");
  String p = prefs.getString("pass", "");
  prefs.end();
  s.toCharArray(wifiSSID,     sizeof(wifiSSID));
  p.toCharArray(wifiPassword, sizeof(wifiPassword));
}

void saveCredentials(const String &ssid, const String &pass) {
  prefs.begin("wifi", false);
  prefs.putString("ssid", ssid);
  prefs.putString("pass", pass);
  prefs.end();
  ssid.toCharArray(wifiSSID,     sizeof(wifiSSID));
  pass.toCharArray(wifiPassword, sizeof(wifiPassword));
}

void enterCredentials() {
  WiFi.disconnect(true);
  saveCredentials(promptInput("SSID:", false), promptInput("Password:", true));
  M5Cardputer.Display.fillScreen(TFT_BLACK);
  M5Cardputer.Display.setTextColor(TFT_GREEN, TFT_BLACK);
  M5Cardputer.Display.setCursor(2, 20);
  M5Cardputer.Display.print("Saved!");
  delay(800);
}

// ==================================================================
//  NETWORKING
// ==================================================================

bool connectWiFi() {
  if (strlen(wifiSSID) == 0) { statusLine = "No credentials - press W"; return false; }
  statusLine = "Connecting WiFi..."; redraw();
  WiFi.mode(WIFI_STA);
  WiFi.begin(wifiSSID, wifiPassword);
  unsigned long start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < 15000) delay(250);
  bool ok = (WiFi.status() == WL_CONNECTED);
  if (ok) {
    configTime(0, 0, "pool.ntp.org", "time.nist.gov");
    statusLine = "WiFi OK  " + WiFi.localIP().toString();
  } else {
    statusLine = "WiFi FAILED - press W";
  }
  return ok;
}

// ---- Fetch all outdoor activation spots from Spothole ----
// Uses streaming JSON parse — avoids loading the full response into a
// String first, which would exhaust the ESP32 heap and cause hangs.
int fetchSpothole() {
  statusLine = "Fetching spots...";
  redraw();

  WiFiClientSecure client;
  client.setInsecure();
  client.setTimeout(15000);

  HTTPClient http;
  http.begin(client, SPOTHOLE_URL);
  http.setTimeout(15000);
  http.addHeader("User-Agent", "M5Cardputer-SpotViewer/1.0");

  int code = http.GET();
  if (code != 200) {
    statusLine = "HTTP err " + String(code);
    http.end();
    return 0;
  }

  String payload = http.getString();
  http.end();

  if (payload.isEmpty()) {
    statusLine = "Empty response";
    return 0;
  }

  StaticJsonDocument<256> filter;
  filter[0]["dx_call"]  = true;
  filter[0]["freq"]     = true;
  filter[0]["mode"]     = true;
  filter[0]["sig"]      = true;
  filter[0]["sig_refs"] = true;
  filter[0]["time"]     = true;
  filter[0]["time_iso"] = true;
  filter[0]["comment"]  = true;

  DynamicJsonDocument doc(32768);
  DeserializationError err = deserializeJson(doc, payload,
                               DeserializationOption::Filter(filter));

  if (err) {
    statusLine = "JSON:" + String(err.c_str()) + " " + payload.substring(0, 20);
    return 0;
  }

  if (!doc.is<JsonArray>()) {
    statusLine = "Not array: " + payload.substring(0, 30);
    return 0;
  }

  int added = 0, sotaCount = 0, otherCount = 0;
  for (JsonObject obj : doc.as<JsonArray>()) {
    if (added >= MAX_SPOTS) break;
    Spot &s = spots[added];

    String sig = obj["sig"].as<String>();
    s.source   = sig.equalsIgnoreCase("SOTA") ? 'S' : 'P';

    s.epoch    = (time_t)obj["time"].as<long>();
    String tiso = obj["time_iso"].as<String>();
    s.time     = (tiso.length() >= 16) ? tiso.substring(11, 16) : "--:--";

    s.callsign = obj["dx_call"].as<String>();
    if (s.callsign.isEmpty() || s.callsign.length() < 3 ||
        s.callsign.equalsIgnoreCase("deprecated") ||
        s.callsign.equalsIgnoreCase("null")) continue;

    char freqBuf[12];
    snprintf(freqBuf, sizeof(freqBuf), "%.3f",
             obj["freq"].as<long>() / 1000000.0);
    s.freq = String(freqBuf);

    s.mode = obj["mode"].as<String>();

    JsonArray refs = obj["sig_refs"].as<JsonArray>();
    if (refs.size() == 0) continue;
    s.reference = refs[0]["id"].as<String>();
    s.name      = refs[0]["name"].as<String>();
    s.region    = sig;
    s.comments  = obj["comment"].as<String>();
    if (s.comments == "null") s.comments = "";

    if (s.source == 'S') sotaCount++; else otherCount++;
    added++;
  }
  return added;
}

// ---- Fetch, sort, rebuild ----
bool fetchAll() {
  if (WiFi.status() != WL_CONNECTED) {
    if (!connectWiFi()) return false;
  }

  spotCount = fetchSpothole();
  sortSpots();
  rebuildVisible();
  lastFetchMs = millis();
  if (statusLine.startsWith("Fetching") || statusLine.startsWith("HTTP") || statusLine.startsWith("JSON"))
    statusLine = "r=refresh f=VK m=mode b=band";
  return true;
}

// ==================================================================
//  KEYBOARD HANDLER
// ==================================================================

void handleKeyboard() {
  if (!M5Cardputer.Keyboard.isChange()) return;
  if (!M5Cardputer.Keyboard.isPressed()) return;

  Keyboard_Class::KeysState st = M5Cardputer.Keyboard.keysState();
  bool needRedraw = false;

  for (char c : st.word) {
    if      (c == ';')              { scrollOffset--;              needRedraw = true; }
    else if (c == '.')              { scrollOffset++;              needRedraw = true; }
    else if (c == ',')              { scrollOffset -= rowsVisible; needRedraw = true; }
    else if (c == '/')              { scrollOffset += rowsVisible; needRedraw = true; }
    else if (c == 'r' || c == 'R') { fetchAll();                  needRedraw = true; }
    else if (c == 'f' || c == 'F') {
      filterVKZL = !filterVKZL;
      rebuildVisible();
      needRedraw = true;
    }
    else if (c == 'm' || c == 'M') {
      modeFilterIdx = (modeFilterIdx + 1) % MODE_COUNT;
      rebuildVisible();
      needRedraw = true;
    }
    else if (c == 'b' || c == 'B') {
      sortByBand = !sortByBand;
      sortSpots();
      rebuildVisible();
      needRedraw = true;
    }
    else if (c == 'w' || c == 'W') {
      enterCredentials();
      fetchAll();
      needRedraw = true;
    }
  }

  if (needRedraw) {
    scrollOffset = constrain(scrollOffset, 0, max(0, visibleCount - rowsVisible));
    redraw();
  }
}

// ==================================================================
//  ARDUINO ENTRY POINTS
// ==================================================================

void setup() {
  auto cfg = M5.config();
  M5Cardputer.begin(cfg, true);
  M5Cardputer.Display.setEpdMode(epd_mode_t::epd_quality);
  esp_log_level_set("*", ESP_LOG_NONE);

  M5Cardputer.Display.setRotation(1);
  M5Cardputer.Display.setTextSize(1);
  M5Cardputer.Display.fillScreen(TFT_BLACK);

  loadCredentials();
  if (strlen(wifiSSID) == 0) enterCredentials();

  redraw();
  fetchAll();
  redraw();
}

void loop() {
  M5Cardputer.update();
  handleKeyboard();

  static unsigned long lastClockMs = 0;
  static unsigned long lastAgeMs   = 0;
  unsigned long now = millis();

  if (now - lastClockMs >= 1000)  { lastClockMs = now; drawHeader(); }
  if (now - lastAgeMs   >= 60000) { lastAgeMs   = now; drawList();   }
  if (now - lastFetchMs > AUTO_REFRESH_MS) { fetchAll(); redraw(); }

  delay(5);
}
