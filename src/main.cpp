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

// --- CONFIGURATION ---
const char* GOOGLE_SCRIPT_URL = "Change here"; 

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

void applyBrightness() {
  // HARDWARE FIX:
  // Your backlight turns off if duty > 250.
  // We clamp the range to 0 (Brightest) -> 240 (Dimmest Visible).
  // This ensures 1% brightness is still visible, not black.
  
  // Map 0-100 Slider to 240-0 Duty Cycle
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
  String html = "<html><head><meta name='viewport' content='width=device-width, initial-scale=1'><style>";
  html += "body{font-family:sans-serif; background:#222; color:#fff; padding:20px;}";
  html += ".btn{background:#007bff; color:white; padding:10px 20px; text-decoration:none; border-radius:5px; margin:5px; border:none; cursor:pointer;}";
  html += ".card{background:#333; padding:15px; border-radius:8px; margin-bottom:15px;}";
  html += "input[type=number]{padding:8px; width:100px;}";
  html += "input[type=range]{width:100%;}";
  html += "</style></head><body><h2>GeekMagic Control</h2>";
  
  html += "<div class='card'><h3>Settings</h3><form action='/set' method='POST'>";
  html += "Timezone Offset: <input type='number' name='offset' value='" + String(gmtOffset) + "'><br><br>";
  
  html += "Brightness (" + String(brightness) + "%): <br>";
  html += "<input type='range' name='bright' min='0' max='100' value='" + String(brightness) + "' onchange='this.form.submit()'><br><br>";
  
  html += "<input type='submit' value='Save' class='btn'></form></div>";
  
  html += "<div class='card'><h3>Update Firmware</h3>";
  html += "<form method='POST' action='/update' enctype='multipart/form-data'>";
  html += "<input type='file' name='update' style='color:#fff'><br><br>";
  html += "<input type='submit' value='Upload .bin' class='btn' style='background:#28a745'>";
  html += "</form></div></body></html>";
  return html;
}

void drawUI() {
  tft.fillRect(0, 30, 240, 210, TFT_BLACK); 
  
  for (int i = 0; i < 8; i++) {
    if (events[i].title.length() > 0) {
      int col = i / 4; 
      int row = i % 4; 
      int x = (col * 120) + 3; 
      int y = 34 + (row * 49); 
      
      tft.fillRoundRect(x, y, 114, 45, 6, events[i].color);
      
      uint16_t textColor = events[i].isDark ? TFT_WHITE : TFT_BLACK;
      uint16_t timeColor = events[i].isDark ? 0xCE79 : 0x3186; 

      tft.setTextColor(textColor, events[i].color);
      tft.setTextDatum(TL_DATUM); 
      tft.drawString(events[i].title.substring(0, 11), x + 5, y + 4, 2);
      
      tft.setTextColor(timeColor, events[i].color);
      tft.drawString(events[i].displayTime, x + 5, y + 26, 2);
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
  time_t now = time(nullptr);
  struct tm* p_tm = localtime(&now);
  char timeStr[10];
  sprintf(timeStr, "%02d:%02d", p_tm->tm_hour, p_tm->tm_min);
  char dateStr[20];
  strftime(dateStr, sizeof(dateStr), "%b %d", p_tm);

  tft.fillRect(0, 0, 240, 30, 0x18E3); 
  tft.setTextColor(TFT_WHITE, 0x18E3);
  tft.setTextDatum(MR_DATUM); 
  tft.drawString(timeStr, 235, 15, 4); 
  tft.setTextDatum(ML_DATUM); 
  tft.drawString(dateStr, 5, 15, 4); 
}

void fetchCalendar() {
  if (WiFi.status() != WL_CONNECTED) { drawError("WiFi Lost"); return; }
  
  std::unique_ptr<WiFiClientSecure>client(new WiFiClientSecure);
  client->setInsecure();
  
  HTTPClient https;
  https.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
  https.setTimeout(15000); 

  if (https.begin(*client, GOOGLE_SCRIPT_URL)) {
    int httpCode = https.GET();
    if (httpCode > 0) {
      String payload = https.getString();
      https.end(); 
      
      DynamicJsonDocument doc(5120); 
      DeserializationError error = deserializeJson(doc, payload);

      if (!error) {
        JsonArray array = doc.as<JsonArray>();
        int count = 0;
        for(int k=0; k<8; k++) events[k].title = ""; 

        for(JsonObject v : array) {
          if(count >= 8) break;
          events[count].title = v["t"].as<String>();
          events[count].displayTime = v["d"].as<String>();
          events[count].color = v["c"].as<uint16_t>();
          events[count].isDark = v["isDark"].as<bool>();
          count++;
        }
        drawUI();
      } else {
        drawError("JSON: " + String(error.c_str()));
      }
    } else {
      drawError("HTTP " + String(httpCode));
      https.end();
    }
  } else {
    drawError("Connect Fail");
  }
}

void setup() {
  Serial.begin(115200);
  loadSettings();

  tft.init();
  tft.setRotation(0);
  tft.fillScreen(TFT_BLACK);
  
  pinMode(TFT_BL, OUTPUT);
  applyBrightness(); 

  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.setTextDatum(MC_DATUM);
  tft.drawString("Connecting...", 120, 120, 4);

  WiFiManager wifiManager;
  wifiManager.autoConnect("GeekMagic_Setup");

  tft.fillScreen(TFT_BLACK);
  configTime(gmtOffset, 0, "pool.ntp.org", "time.google.com");

  server.on("/", []() { server.send(200, "text/html", getHtml()); });
  
  server.on("/set", HTTP_POST, []() {
      if (server.hasArg("offset")) gmtOffset = server.arg("offset").toInt();
      if (server.hasArg("bright")) brightness = server.arg("bright").toInt();
      
      saveSettings();
      applyBrightness(); 
      configTime(gmtOffset, 0, "pool.ntp.org", "time.google.com");
      
      // AUTO RELOAD - No "Saved" page
      server.sendHeader("Location", "/");
      server.send(303);
      
      drawClock();
  });

  server.on("/update", HTTP_POST, []() {
    server.send(200, "text/plain", (Update.hasError()) ? "Update Failed" : "Update Success! Rebooting...");
    ESP.restart();
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
  if (millis() - lastClock > 1000) {
    lastClock = millis();
    drawClock();
  }

  if (millis() - lastFetchTime > fetchInterval) {
    lastFetchTime = millis();
    fetchCalendar();
  }
}