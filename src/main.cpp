#include <Arduino.h>
#include <ESP8266WiFi.h>
#include <ESP8266WebServer.h>
#include <WiFiManager.h> 
#include <TFT_eSPI.h>
#include <ESP8266HTTPClient.h>
#include <WiFiClientSecure.h>
#include <ArduinoJson.h>
#include <time.h>
#include <EEPROM.h>
#include <Updater.h>
#include <stdarg.h>
#include <WiFiUdp.h>
#include <sys/time.h>

// --- CONFIGURATION ---
// Put your real URL in include/secrets.h (git-ignored): #define GOOGLE_SCRIPT_URL_SECRET "https://..."
#if __has_include("secrets.h")
#include "secrets.h"
const char* GOOGLE_SCRIPT_URL = GOOGLE_SCRIPT_URL_SECRET;
#else
const char* GOOGLE_SCRIPT_URL = "https://script.google.com/macros/s/changethis/exec";
#endif

TFT_eSPI tft = TFT_eSPI();
ESP8266WebServer server(80);

unsigned long lastFetchTime = 0;
const unsigned long fetchInterval = 120000; 

struct Event {
  String title;
  String displayTime;
  uint16_t color;
  bool isDark;
};
Event events[8];

long gmtOffset = 28800; 
int brightness = 100; 

// --- LOGGING (ring buffer, readable at http://<ip>/log) ---
#define LOG_SIZE 4096
char logBuf[LOG_SIZE];
size_t logLen = 0;

void logf(const char* fmt, ...) {
  char line[160];
  int n = snprintf(line, sizeof(line), "[%8lu] ", millis());
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(line + n, sizeof(line) - n - 2, fmt, ap);
  va_end(ap);
  strcat(line, "\n");
  Serial.print(line);
  size_t len = strlen(line);
  if (logLen + len >= LOG_SIZE) { // drop oldest half
    size_t keep = LOG_SIZE / 2;
    memmove(logBuf, logBuf + logLen - keep, keep);
    logLen = keep;
  }
  memcpy(logBuf + logLen, line, len);
  logLen += len;
  logBuf[logLen] = 0;
}

String lastFetchResult = "not yet";

// --- NTP TIME ---
const char* NTP_SERVER_1 = "time.google.com";
const char* NTP_SERVER_2 = "time.cloudflare.com";
const char* NTP_SERVER_3 = "pool.ntp.org";
const time_t MIN_VALID_TIME = 1700000000; // anything earlier means NTP has not synced yet

bool timeIsSynced() {
  return time(nullptr) > MIN_VALID_TIME;
}

// Fallback when NTP is blocked/slow: set the clock from an HTTP "Date" header (UTC)
bool setTimeFromHttpDate(const String& d) {
  static const char* mon = "JanFebMarAprMayJunJulAugSepOctNovDec";
  int day, year, hh, mm, ss; char m[4] = {0};
  if (sscanf(d.c_str(), "%*3s, %d %3s %d %d:%d:%d", &day, m, &year, &hh, &mm, &ss) != 6) return false;
  const char* f = strstr(mon, m);
  if (!f) return false;
  int month = (f - mon) / 3 + 1;
  // days since 1970-01-01 (civil-from-days algorithm)
  int y = year - (month <= 2);
  int era = (y >= 0 ? y : y - 399) / 400;
  unsigned yoe = y - era * 400;
  unsigned doy = (153 * (month + (month > 2 ? -3 : 9)) + 2) / 5 + day - 1;
  unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  long days = (long)era * 146097 + (long)doe - 719468;
  struct timeval tv = { (time_t)(days * 86400L + hh * 3600L + mm * 60L + ss), 0 };
  settimeofday(&tv, nullptr);
  return true;
}

// Direct UDP NTP query: fast, and logs why it fails (lwIP's SNTP gave no feedback)
bool ntpOk = false; // true once a real NTP reply was received

bool ntpQuery(const char* host) {
  IPAddress ip;
  unsigned long t0 = millis();
  if (!WiFi.hostByName(host, ip, 2000)) { logf("NTP: DNS failed for %s", host); return false; }
  WiFiUDP udp;
  udp.begin(2390);
  uint8_t pkt[48] = {0};
  pkt[0] = 0x1B; // LI=0, v3, client mode
  udp.beginPacket(ip, 123);
  udp.write(pkt, 48);
  udp.endPacket();
  for (int i = 0; i < 20; i++) { // up to 2s
    delay(100);
    if (udp.parsePacket() >= 48) {
      udp.read(pkt, 48);
      udp.stop();
      uint32_t secs = ((uint32_t)pkt[40] << 24) | (pkt[41] << 16) | (pkt[42] << 8) | pkt[43];
      struct timeval tv = { (time_t)(secs - 2208988800UL), 0 };
      settimeofday(&tv, nullptr);
      ntpOk = true;
      logf("NTP: %s (%s) ok in %lu ms", host, ip.toString().c_str(), millis() - t0);
      return true;
    }
  }
  udp.stop();
  logf("NTP: %s (%s) no reply", host, ip.toString().c_str());
  return false;
}

bool ntpSyncNow() {
  return ntpQuery(NTP_SERVER_1) || ntpQuery(NTP_SERVER_2) || ntpQuery(NTP_SERVER_3);
}

void saveSettings();

// Timezone follows the device's location: ask ip-api.com for the UTC offset of our public IP
bool updateTimezone() {
  WiFiClient c;
  HTTPClient http;
  http.setTimeout(5000);
  if (!http.begin(c, "http://ip-api.com/line/?fields=offset")) return false;
  int code = http.GET();
  if (code == 200) {
    String body = http.getString();
    body.trim();
    long off = body.toInt();
    if (body.length() > 0 && off > -43200 && off < 50400) {
      if (off != gmtOffset) { gmtOffset = off; saveSettings(); }
      logf("TZ: offset %ld s from IP location", off);
      http.end();
      return true;
    }
  }
  logf("TZ: lookup failed (HTTP %d), keeping %ld s", code, gmtOffset);
  http.end();
  return false;
}

void startNtp() {
  configTime(gmtOffset, 0, NTP_SERVER_1, NTP_SERVER_2, NTP_SERVER_3);
}

void applyBrightness() {
  int duty = map(brightness, 0, 100, 240, 0);
  analogWrite(TFT_BL, duty);
}

void saveSettings() {
  EEPROM.begin(512);
  EEPROM.put(0, gmtOffset);
  EEPROM.put(10, brightness);
  EEPROM.commit();
  EEPROM.end();
}

void loadSettings() {
  EEPROM.begin(512);
  long tempOffset;
  int tempBright;
  EEPROM.get(0, tempOffset);
  EEPROM.get(10, tempBright);
  if (tempOffset > -43200 && tempOffset < 50400) gmtOffset = tempOffset;
  if (tempBright >= 0 && tempBright <= 100) brightness = tempBright;
  EEPROM.end();
}

String getHtml() {
  String html = F("<!doctype html><html><head><meta charset='utf-8'><meta name='viewport' content='width=device-width, initial-scale=1'><title>GeekMagic</title><style>"
    ":root{--bg:#0d1117;--card:#161b22;--line:#30363d;--text:#e6edf3;--mute:#8b949e;--acc:#2f81f7;--ok:#238636}"
    "*{box-sizing:border-box}body{margin:0;padding:24px 16px;background:var(--bg);color:var(--text);font:15px/1.5 -apple-system,Segoe UI,Roboto,sans-serif}"
    ".wrap{max-width:460px;margin:0 auto}h1{font-size:22px;margin:0 0 4px}.sub{color:var(--mute);margin-bottom:20px;font-size:13px}"
    ".card{background:var(--card);border:1px solid var(--line);border-radius:12px;padding:18px;margin-bottom:16px}"
    "h3{margin:0 0 14px;font-size:13px;text-transform:uppercase;letter-spacing:.06em;color:var(--mute)}"
    "label{display:block;margin:12px 0 6px;font-size:13px;color:var(--mute)}"
    "input[type=number],input[type=file]{width:100%;padding:10px 12px;background:var(--bg);border:1px solid var(--line);border-radius:8px;color:var(--text)}"
    "input[type=range]{width:100%;accent-color:var(--acc)}"
    ".btn{width:100%;margin-top:16px;padding:11px;border:0;border-radius:8px;background:var(--acc);color:#fff;font-size:15px;font-weight:600;cursor:pointer}"
    ".btn.ok{background:var(--ok)}.btn.ghost{background:transparent;border:1px solid var(--line);color:var(--text)}"
    ".row{display:flex;justify-content:space-between;color:var(--mute);font-size:13px;padding:4px 0}.row b{color:var(--text);font-weight:500}"
    "</style></head><body><div class='wrap'><h1>GeekMagic</h1><div class='sub'>Calendar display control</div>");
  html += F("<div class='card'><h3>Status</h3><div id='st'><div class='row'>Loading...</div></div>"
    "<a href='/log' target='_blank'><button class='btn ghost' type='button'>View log</button></a></div>");
  html += "<div class='card'><h3>Settings</h3><form action='/set' method='POST'>";
  html += "<label>Brightness <b id='bv'>" + String(brightness) + "%</b></label>";
  html += "<input type='range' name='bright' min='0' max='100' value='" + String(brightness) + "' oninput=\"bv.textContent=this.value+'%'\" onchange='this.form.submit()'>";
  html += "<button class='btn' type='submit'>Save</button></form></div>";
  html += F("<div class='card'><h3>Firmware update</h3><form method='POST' action='/update' enctype='multipart/form-data'>"
    "<input type='file' name='update' accept='.bin'><button class='btn ok' type='submit'>Upload .bin</button></form></div></div>"
    "<script>fetch('/status').then(r=>r.json()).then(j=>{const m=s=>Math.floor(s/60)+'m';"
    "st.innerHTML=[['WiFi',j.ssid+' ('+j.rssi+' dBm)'],['Time synced',j.time_synced?'yes':'no'],['Last fetch',j.last_fetch],['Uptime',m(j.uptime_s)],['Free heap',j.heap+' B']]"
    ".map(x=>\"<div class='row'>\"+x[0]+\"<b>\"+x[1]+\"</b></div>\").join('')}).catch(()=>{});</script></body></html>");
  return html;
}

void drawUI() {
  tft.fillRect(0, 30, 240, 210, TFT_BLACK); 
  
  for (int i = 0; i < 8; i++) {
    if (events[i].title.length() > 0) {
      int col = i / 4; 
      int row = i % 4; 
      int x = (col * 120) + 3; 
      int y = 32 + (row * 52); 
      
      tft.fillRoundRect(x, y, 114, 50, 6, events[i].color);
      
      // Determine Text Color (White or Black)
      uint16_t textColor = events[i].isDark ? TFT_WHITE : TFT_BLACK;

      // 1. TITLE
      tft.setTextColor(textColor, events[i].color); // Use Pure Color
      tft.setTextDatum(TL_DATUM); 
      
      String t = events[i].title.substring(0, 8); 
      // MOVED DOWN: y+4 (was y+2)
      tft.drawString(t, x + 4, y + 4, 4); 
      
      // 2. TIME
      tft.setTextColor(textColor, events[i].color); // Use Same Pure Color
      
      // MOVED DOWN: y+30 (was y+28) to avoid overlapping the lower title
      tft.drawString(events[i].displayTime, x + 5, y + 30, 2);
    }
  }
}

void drawError(String msg) {
  tft.fillRect(0, 30, 240, 210, TFT_BLACK);
  tft.setTextColor(TFT_RED, TFT_BLACK);
  tft.setTextDatum(MC_DATUM);
  tft.drawString(msg, 120, 120, 4);
}

void drawClock() {
  char timeStr[10];
  char dateStr[20];
  if (timeIsSynced()) {
    time_t now = time(nullptr);
    struct tm* p_tm = localtime(&now);
    int h12 = p_tm->tm_hour % 12;
    sprintf(timeStr, "%d:%02d %s", h12 == 0 ? 12 : h12, p_tm->tm_min, p_tm->tm_hour < 12 ? "AM" : "PM");
    strftime(dateStr, sizeof(dateStr), "%b %d", p_tm);
  } else {
    strcpy(timeStr, "--:-- --");
    strcpy(dateStr, "Syncing");
  }

  tft.fillRect(0, 0, 240, 30, 0x18E3); 
  tft.setTextColor(TFT_WHITE, 0x18E3);
  tft.setTextDatum(MR_DATUM); 
  tft.drawString(timeStr, 235, 15, 4); 
  tft.setTextDatum(ML_DATUM); 
  tft.drawString(dateStr, 5, 15, 4); 
}

void fetchCalendar() {
  if (WiFi.status() != WL_CONNECTED) { lastFetchResult = "WiFi lost"; logf("Fetch: WiFi lost"); drawError("WiFi Lost"); return; }
  unsigned long t0 = millis();
  
  String payload;
  int httpCode = 0;
  bool began = false;
  {
    // Scoped so the TLS client (~20KB of RAM) is freed before JSON parsing
    std::unique_ptr<WiFiClientSecure> client(new WiFiClientSecure);
    client->setInsecure();

    HTTPClient https;
    https.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
    https.setTimeout(15000);
    const char* hdrs[] = {"Date"};
    https.collectHeaders(hdrs, 1);

    if (https.begin(*client, GOOGLE_SCRIPT_URL)) {
      began = true;
      httpCode = https.GET();
      if (httpCode > 0) {
        payload = https.getString();
        if (!ntpOk && setTimeFromHttpDate(https.header("Date"))) logf("Time: set from HTTP Date header");
      }
      https.end();
    }
  }

  if (!began) {
    lastFetchResult = "connect fail";
    logf("Fetch: connect fail");
    drawError("Connect Fail");
    return;
  }
  if (httpCode <= 0) {
    lastFetchResult = "HTTP error " + String(httpCode);
    logf("Fetch: HTTP error %d", httpCode);
    drawError("HTTP " + String(httpCode));
    return;
  }

  DynamicJsonDocument doc(3072);
  DeserializationError error = deserializeJson(doc, payload);
  lastFetchResult = "HTTP " + String(httpCode) + ", " + (error ? String("JSON ") + error.c_str() : String("ok")) + ", " + String(millis() - t0) + " ms";
  logf("Fetch: %s (%u bytes, heap %u)", lastFetchResult.c_str(), payload.length(), ESP.getFreeHeap());
  if (error) { drawError("JSON: " + String(error.c_str())); return; }

  JsonArray array = doc.as<JsonArray>();
  int count = 0;
  for (int k = 0; k < 8; k++) events[k].title = "";
  for (JsonObject v : array) {
    if (count >= 8) break;
    events[count].title = v["t"].as<String>();
    events[count].displayTime = v["d"].as<String>();
    events[count].color = v["c"].as<uint16_t>();
    events[count].isDark = v["isDark"].as<bool>();
    count++;
  }
  drawUI();
}

void setup() {
  Serial.begin(115200);
  logf("Boot: reset reason '%s'", ESP.getResetReason().c_str());
  loadSettings();
  WiFi.persistent(true);      // keep credentials in flash
  WiFi.setAutoReconnect(true);
  WiFi.mode(WIFI_STA);
  tft.init();
  tft.setRotation(0);
  tft.fillScreen(TFT_BLACK);
  pinMode(TFT_BL, OUTPUT);
  applyBrightness(); 
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.setTextDatum(MC_DATUM);
  tft.drawString("Connecting...", 120, 120, 4);

  WiFiManager wifiManager;
  wifiManager.setSaveConfigCallback([]() {
    // User just entered WiFi details in the portal - reboot so they take effect cleanly
    logf("WiFi: credentials entered, restarting");
    delay(1000);
    ESP.restart();
  });
  wifiManager.setConnectTimeout(30);        // slow routers / soft reboot after OTA need time
  wifiManager.setConfigPortalTimeout(180);  // never sit in the portal forever
  wifiManager.setAPCallback([](WiFiManager*) {
    // Saved WiFi failed - device is now hosting its own setup network
    tft.fillScreen(TFT_BLACK);
    tft.setTextColor(TFT_YELLOW, TFT_BLACK);
    tft.setTextDatum(MC_DATUM);
    tft.drawString("Setup WiFi", 120, 70, 4);
    tft.setTextColor(TFT_WHITE, TFT_BLACK);
    tft.drawString("Join network:", 120, 120, 2);
    tft.drawString("GeekMagic_Setup", 120, 145, 4);
    tft.drawString("then open 192.168.4.1", 120, 185, 2);
  });
  logf("WiFi: connecting (saved SSID '%s')", WiFi.SSID().c_str());
#ifdef WIFI_SSID_SECRET
  // Built-in credentials survive any OTA/flash wipe; the portal is only the last resort
  WiFi.begin(WIFI_SSID_SECRET, WIFI_PASS_SECRET);
  for (int i = 0; i < 200 && WiFi.status() != WL_CONNECTED; i++) delay(100);
  logf("WiFi: built-in credentials %s", WiFi.status() == WL_CONNECTED ? "ok" : "failed");
#endif
  if (!wifiManager.autoConnect("GeekMagic_Setup")) {
    logf("WiFi: portal timed out, restarting to retry saved WiFi");
    delay(500);
    ESP.restart();
  }
  logf("WiFi: connected %s rssi=%d ip=%s", WiFi.SSID().c_str(), WiFi.RSSI(), WiFi.localIP().toString().c_str());

  tft.fillScreen(TFT_BLACK);
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.setTextDatum(MC_DATUM);
  tft.drawString("Syncing time...", 120, 120, 4);
  updateTimezone();
  startNtp();
  unsigned long ntpStart = millis();
  ntpQuery(NTP_SERVER_1); // one quick try; if blocked, the HTTP Date header sets the clock on first fetch
  logf("NTP: %s after %lu ms", timeIsSynced() ? "synced" : "pending (HTTP Date fallback)", millis() - ntpStart);
  tft.fillScreen(TFT_BLACK);

  server.on("/", []() { server.send(200, "text/html", getHtml()); });
  server.on("/log", []() { server.send(200, "text/plain", logBuf); });
  server.on("/status", []() {
    String j = "{\"uptime_s\":" + String(millis() / 1000);
    j += ",\"heap\":" + String(ESP.getFreeHeap());
    j += ",\"rssi\":" + String(WiFi.RSSI());
    j += ",\"ssid\":\"" + WiFi.SSID() + "\"";
    j += ",\"time_synced\":" + String(timeIsSynced() ? "true" : "false");
    j += ",\"epoch\":" + String((long)time(nullptr));
    j += ",\"last_fetch\":\"" + lastFetchResult + "\"";
    j += ",\"reset_reason\":\"" + ESP.getResetReason() + "\"}";
    server.send(200, "application/json", j);
  });
  server.on("/restart", []() { server.send(200, "text/plain", "Restarting"); delay(300); ESP.restart(); });
  server.on("/set", HTTP_POST, []() {
      if (server.hasArg("bright")) brightness = server.arg("bright").toInt();
      saveSettings();
      applyBrightness(); 
      startNtp();
      server.sendHeader("Location", "/");
      server.send(303);
      drawClock();
  });

  server.on("/update", HTTP_POST, []() {
    bool failed = Update.hasError();
    server.sendHeader("Connection", "close");
    server.send(200, "text/plain", failed ? "Update Failed" : "Update Success! Rebooting...");
    if (!failed) {
      delay(500); // let the response flush before rebooting
      ESP.restart();
    }
  }, []() {
    HTTPUpload& upload = server.upload();
    if (upload.status == UPLOAD_FILE_START) {
      uint32_t maxSketchSpace = (ESP.getFreeSketchSpace() - 0x1000) & 0xFFFFF000;
      if (!Update.begin(maxSketchSpace)) Update.printError(Serial);
    } else if (upload.status == UPLOAD_FILE_WRITE) {
      if (Update.write(upload.buf, upload.currentSize) != upload.currentSize) Update.printError(Serial);
    } else if (upload.status == UPLOAD_FILE_END) {
      if (Update.end(true)) Serial.printf("Update Success: %u\n", upload.totalSize);
      else Update.printError(Serial);
    }
  });
  server.begin();
  drawUI();
  fetchCalendar();
}

void loop() {
  server.handleClient();
  static unsigned long lastClock = 0;
  static unsigned long lastNtpRetry = 0;
  if (!ntpOk && WiFi.status() == WL_CONNECTED && millis() - lastNtpRetry > 600000UL) {
    lastNtpRetry = millis();
    ntpQuery(NTP_SERVER_1); // retry NTP every 10 min; meanwhile fetches keep time via HTTP Date
  }
  static unsigned long lastTz = 0;
  if (WiFi.status() == WL_CONNECTED && millis() - lastTz > 6UL * 3600000UL) {
    lastTz = millis();
    if (updateTimezone()) startNtp(); // re-apply offset if we moved
  }
  static unsigned long lastResync = 0;
  if (ntpOk && millis() - lastResync > 3600000UL) {
    lastResync = millis();
    ntpQuery(NTP_SERVER_1); // hourly drift correction
  }
  if (millis() - lastClock > 1000) {
    lastClock = millis();
    drawClock();
  }
  if (millis() - lastFetchTime > fetchInterval) {
    lastFetchTime = millis();
    fetchCalendar();
  }
}