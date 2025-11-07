#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <WiFiClient.h>
#include <HTTPClient.h>
#include <WebServer.h>
#include <FS.h>
#include <SPIFFS.h>
#include <ArduinoJson.h>
#include <ArduinoOTA.h>
#include <Update.h>
#include <time.h>
#include <Adafruit_NeoPixel.h>

// ================== BUILD INFO ==================
#define FIRMWARE_VERSION "1.2.0-rgb"

// ================== TLS SETTINGS ==================
// 1 = lewati verifikasi sertifikat (dev/test)
// 0 = verifikasi TLS dg ROOT_CA (production)
#define USE_INSECURE_TLS 1

static const char* ROOT_CA = R"(-----BEGIN CERTIFICATE-----
MIIF...REPLACE_WITH_YOUR_CA_CHAIN...==
-----END CERTIFICATE-----
)";

// ================== SERVER CONFIG ==================
#define SERVER_VALIDATE_BASE   "http://project.sstech.id/IOT/api/validate"
#define AUTH_LOGIN_URL         "http://project.sstech.id/IOT/api/auth/login"
#define AUTH_REFRESH_URL       "http://project.sstech.id/IOT/api/auth/refresh"
#define HEARTBEAT_URL          "https://project.sstech.id/IOT/api/device/heartbeat"   // disarankan HTTPS
#define DEVICE_PSK             "super-secret-device-psk"

// ================== PIN & HARDWARE ==================
#define RELAY_PIN     36   // relay active LOW (ubah ke 4/33 jika perlu)
#define QR_RX       16     // UART2 RX
#define QR_TX       17     // UART2 TX
#define UNLOCK_MS   5000UL

// RGB NeoPixel (WS2812) bawaan board
#define RGB_PIN 48
Adafruit_NeoPixel rgb(1, RGB_PIN, NEO_GRB + NEO_KHZ800);

// ================== TIMERS ==================
#define SYNC_INTERVAL_MS        (10UL * 60UL * 1000UL)
#define PUSH_INTERVAL_MS        (30UL * 1000UL)
#define LOGPULL_INTERVAL_MS     (60UL * 60UL * 1000UL)
#define QR_DEBOUNCE_MS          1500UL
#define WIFI_FAIL_PORTAL_AFTER  (2UL * 60UL * 1000UL)  // 2 menit putus → portal
#define PORTAL_TIMEOUT_MS       (5UL * 60UL * 1000UL)  // 5 menit portal
#define HEARTBEAT_INTERVAL_MS   (30UL * 1000UL)

// NTP (WIB)
#define TZ_OFFSET (7 * 3600)
#define DST_OFFSET 0

// ================== GLOBALS ==================
HardwareSerial QRSerial(2);
WebServer configServer(80);

char deviceId[32];
bool wifiAvailable = false;
bool doorUnlocked = false;
unsigned long unlockStart = 0;

static const char* WIFI_CFG_PATH = "/wifi.json";
static const char* TOKENS_PATH   = "/tokens.sec";    // encrypted
static const char* CREDENT_PATH  = "/credent.sec";   // encrypted
static const char* CONFIG_AP_PASS = "setup123";

// JSON buffers
DynamicJsonDocument allowedList(4096);
DynamicJsonDocument logBuffer(8192);

// tokens
struct Tokens {
  String access;
  String refresh;
  long accessExp;
  long refreshExp;
} TOK;

// wifi cfg
struct WifiCredentials {
  String ssid;
  String password;
} wifiCfg;

// last ticks
unsigned long lastSync = 0;
unsigned long lastPush = 0;
unsigned long lastLogPull = 0;
unsigned long wifiDownSince = 0;
unsigned long lastHeartbeat = 0;

// QR anti-spam
String lastQrData = "";
unsigned long lastQrTs = 0;

// ===== forward declares
void startConfigPortal(bool withTimeout = true);
bool ensureAccessToken();
void handleBackendCommand(const String &resp);
bool startOTAFromURL(const String &url, const String &md5);
void sendHeartbeat();

// ================== LED RGB STATE MACHINE ==================
enum LedMode {
  LED_RGB_OFF,
  LED_RGB_ON_GREEN,
  LED_RGB_ON_RED,
  LED_RGB_ON_BLUE,
  LED_RGB_BLINK_SLOW_GREEN,
  LED_RGB_BLINK_FAST_RED,
  LED_RGB_FLASH_OPEN_CYAN,
  LED_RGB_BLINK_FAST_BLUE     // OTA progress
};
LedMode ledMode = LED_RGB_OFF;
unsigned long ledLastToggle = 0;
bool ledOn = false;

void rgbSet(uint8_t r, uint8_t g, uint8_t b) {
  rgb.setPixelColor(0, rgb.Color(r,g,b));
  rgb.show();
}
void rgbOff() {
  rgb.clear();
  rgb.show();
}
void setLedMode(LedMode m) {
  ledMode = m;
  ledLastToggle = 0;
  ledOn = false;
}
void applyLedRGB() {
  unsigned long now = millis();
  switch (ledMode) {
    case LED_RGB_OFF: rgbOff(); break;
    case LED_RGB_ON_GREEN: rgbSet(0,255,0); break;
    case LED_RGB_ON_RED:   rgbSet(255,0,0); break;
    case LED_RGB_ON_BLUE:  rgbSet(0,0,255); break;

    case LED_RGB_BLINK_SLOW_GREEN:
      if (now - ledLastToggle > 600) {
        ledOn = !ledOn;
        if (ledOn) rgbSet(0,255,0); else rgbOff();
        ledLastToggle = now;
      }
      break;

    case LED_RGB_BLINK_FAST_RED:
      if (now - ledLastToggle > 200) {
        ledOn = !ledOn;
        if (ledOn) rgbSet(255,0,0); else rgbOff();
        ledLastToggle = now;
      }
      break;

    case LED_RGB_FLASH_OPEN_CYAN:
      if (now - ledLastToggle > 100) {
        ledOn = !ledOn;
        if (ledOn) rgbSet(0,255,255); else rgbOff();
        ledLastToggle = now;
      }
      break;

    case LED_RGB_BLINK_FAST_BLUE:
      if (now - ledLastToggle > 150) {
        ledOn = !ledOn;
        if (ledOn) rgbSet(0,0,255); else rgbOff();
        ledLastToggle = now;
      }
      break;
  }
}

// ================== SPIFFS SAFE ==================
void safeSPIFFS() {
  if (!SPIFFS.begin(false)) {
    Serial.println("⚠️ SPIFFS mount failed. Formatting SPIFFS...");
    if (!SPIFFS.begin(true)) {
      Serial.println("❌ SPIFFS format/mount failed");
    } else {
      Serial.println("✅ SPIFFS formatted and mounted");
    }
  }
}

// ================== DEVICE ID ==================
void generateDeviceId() {
  uint64_t chipid = ESP.getEfuseMac();
  sprintf(deviceId, "MYRP-%04X%08X", (uint16_t)(chipid >> 32), (uint32_t)chipid);
}

// ================== XOR-OBFUSCATION (ringan) ==================
uint32_t djb2(const String &s) { uint32_t h=5381; for(size_t i=0;i<s.length();i++) h=((h<<5)+h)+(uint8_t)s[i]; return h; }
void xorBuffer(uint8_t* b, size_t n, uint32_t k){ for(size_t i=0;i<n;i++){ uint8_t m=(k>>((i%4)*8))&0xFF; b[i]^=m; } }
bool saveEncrypted(const char* path, const String &plain, uint32_t key) {
  File f = SPIFFS.open(path, "w"); if (!f) return false;
  std::unique_ptr<uint8_t[]> tmp(new uint8_t[plain.length()]);
  memcpy(tmp.get(), plain.c_str(), plain.length()); xorBuffer(tmp.get(), plain.length(), key);
  size_t w = f.write(tmp.get(), plain.length()); f.close(); return w == plain.length();
}
bool loadEncrypted(const char* path, String &out, uint32_t key) {
  if (!SPIFFS.exists(path)) return false; File f = SPIFFS.open(path, "r"); if (!f) return false;
  size_t len=f.size(); std::unique_ptr<uint8_t[]> buf(new uint8_t[len+1]); size_t r=f.read(buf.get(), len); f.close();
  if (r!=len) return false; xorBuffer(buf.get(), len, key); buf[len]=0; out=String((char*)buf.get()); return true;
}

// ================== WIFI CFG ==================
bool wifiConfigValid(){ return wifiCfg.ssid.length()>0; }
void saveWifiConfig() {
  DynamicJsonDocument d(256); d["ssid"]=wifiCfg.ssid; d["password"]=wifiCfg.password;
  File f=SPIFFS.open(WIFI_CFG_PATH,"w"); if(!f){Serial.println("save wifi config failed"); return;}
  serializeJson(d,f); f.close(); Serial.println("✅ WiFi config saved");
}
void loadWifiConfig() {
  wifiCfg.ssid=""; wifiCfg.password="";
  if (!SPIFFS.exists(WIFI_CFG_PATH)) return;
  File f=SPIFFS.open(WIFI_CFG_PATH,"r"); if(!f) return;
  DynamicJsonDocument d(256);
  if (deserializeJson(d,f)==DeserializationError::Ok) {
    wifiCfg.ssid=d["ssid"].as<String>(); wifiCfg.password=d["password"].as<String>();
    Serial.printf("✅ Loaded WiFi config (SSID: %s)\n", wifiCfg.ssid.c_str());
  }
  f.close();
}

// ================== CONFIG PORTAL ==================
void startConfigPortal(bool withTimeout) {
  String apName = String("Setup-") + deviceId;
  WiFi.mode(WIFI_AP);
  WiFi.softAP(apName.c_str(), CONFIG_AP_PASS);
  Serial.printf("⚡ Config Portal: SSID=%s URL=http://%s\n", apName.c_str(), WiFi.softAPIP().toString().c_str());
  setLedMode(LED_RGB_BLINK_FAST_RED); // portal = merah cepat

  configServer.on("/", HTTP_GET, [](){
    String html = "<html><body><h2>Setup WiFi</h2>"
                  "<form method='POST' action='/save'>SSID:<br/><input name='ssid' required/><br/>"
                  "Password:<br/><input name='password' type='password'/><br/><br/>"
                  "<button type='submit'>Save</button></form>"
                  "<p>Device: " + String(deviceId) + "</p></body></html>";
    configServer.send(200,"text/html",html);
  });
  configServer.on("/save", HTTP_POST, [](){
    wifiCfg.ssid = configServer.arg("ssid");
    wifiCfg.password = configServer.arg("password");
    saveWifiConfig();
    configServer.send(200,"text/plain","Saved. Restarting...");
    delay(700); ESP.restart();
  });
  configServer.begin();

  const unsigned long start = millis();
  while (!withTimeout || (millis()-start < PORTAL_TIMEOUT_MS)) {
    configServer.handleClient(); applyLedRGB(); delay(10);
  }
  Serial.println("⏳ Config portal timeout → restarting..."); ESP.restart();
}

// ================== HTTP BEGIN (TLS optional) ==================
bool httpBeginSecure(HTTPClient &http, const String &url) {
  if (url.startsWith("https://")) {
    static WiFiClientSecure *client = nullptr;
    if (!client) {
      client = new WiFiClientSecure();
#if USE_INSECURE_TLS
      client->setInsecure();
#else
      client->setCACert(ROOT_CA);
#endif
      client->setTimeout(15000);
    }
    return http.begin(*client, url);
  } else {
    static WiFiClient *client = nullptr;
    if (!client) client = new WiFiClient();
    return http.begin(*client, url);
  }
}

// ================== TOKENS (ENCRYPTED) ==================
void saveTokens() {
  DynamicJsonDocument d(512);
  d["access"]=TOK.access; d["refresh"]=TOK.refresh; d["accessExp"]=TOK.accessExp; d["refreshExp"]=TOK.refreshExp;
  String js; serializeJson(d, js);
  const uint32_t key = djb2(String(deviceId)+DEVICE_PSK);
  if (saveEncrypted(TOKENS_PATH, js, key)) Serial.println("✅ Tokens saved (enc)"); else Serial.println("❌ Save tokens failed");
}
void loadTokens() {
  const uint32_t key = djb2(String(deviceId)+DEVICE_PSK);
  String js; if (!loadEncrypted(TOKENS_PATH, js, key)) return;
  DynamicJsonDocument d(512);
  if (deserializeJson(d, js)==DeserializationError::Ok) {
    TOK.access=d["access"].as<String>(); TOK.refresh=d["refresh"].as<String>();
    TOK.accessExp=(long)(d["accessExp"]|0); TOK.refreshExp=(long)(d["refreshExp"]|0);
    Serial.println("✅ Tokens loaded (enc)");
  }
}

// ================== ALLOWED LIST (ENCRYPTED) ==================
void saveAllowedList() {
  String js; serializeJson(allowedList, js);
  const uint32_t key = djb2(String(deviceId)+DEVICE_PSK);
  if (saveEncrypted(CREDENT_PATH, js, key)) Serial.println("✅ Saved credent (enc)"); else Serial.println("❌ Save credent failed");
}
void loadLocalAllowed() {
  const uint32_t key = djb2(String(deviceId)+DEVICE_PSK);
  String js; if (!loadEncrypted(CREDENT_PATH, js, key)) {
    Serial.println("No credent.sec → empty list"); allowedList.clear(); allowedList.to<JsonArray>(); return;
  }
  if (deserializeJson(allowedList, js)!=DeserializationError::Ok) {
    Serial.println("Parse credent failed"); allowedList.clear(); allowedList.to<JsonArray>();
  } else {
    Serial.printf("✅ Loaded allowed list (%d items)\n", allowedList.size());
  }
}

// ================== AUTH ==================
bool doLogin() {
  if (WiFi.status()!=WL_CONNECTED) return false;
  HTTPClient http; if (!httpBeginSecure(http, AUTH_LOGIN_URL)) return false;
  http.addHeader("Content-Type","application/json");
  DynamicJsonDocument d(256); d["device_id"]=deviceId; d["psk"]=DEVICE_PSK;
  String body; serializeJson(d, body);
  int code=http.POST(body);
  if (code<200||code>=300){ Serial.printf("Login FAIL %d\n",code); http.end(); return false; }
  String resp=http.getString(); http.end();
  DynamicJsonDocument r(1024);
  if (deserializeJson(r, resp)!=DeserializationError::Ok){ Serial.println("Login parse fail"); return false; }
  TOK.access=r["access_token"].as<String>(); TOK.refresh=r["refresh_token"].as<String>();
  TOK.accessExp=(long)(r["access_exp"]|0); TOK.refreshExp=(long)(r["refresh_exp"]|0);
  saveTokens(); Serial.println("✅ Login OK"); return true;
}
bool doRefresh() {
  if (WiFi.status()!=WL_CONNECTED || TOK.refresh.length()==0) return false;
  HTTPClient http; if (!httpBeginSecure(http, AUTH_REFRESH_URL)) return false;
  http.addHeader("Content-Type","application/json");
  DynamicJsonDocument d(256); d["refresh_token"]=TOK.refresh; String body; serializeJson(d, body);
  int code=http.POST(body); if (code<200||code>=300){ Serial.printf("Refresh FAIL %d\n",code); http.end(); return false; }
  String resp=http.getString(); http.end();
  DynamicJsonDocument r(1024); if (deserializeJson(r, resp)!=DeserializationError::Ok){ Serial.println("Refresh parse fail"); return false; }
  TOK.access=r["access_token"].as<String>(); TOK.accessExp=(long)(r["access_exp"]|0);
  if (r.containsKey("refresh_token")){ TOK.refresh=r["refresh_token"].as<String>(); TOK.refreshExp=(long)(r["refresh_exp"]|0); }
  saveTokens(); Serial.println("✅ Refresh OK"); return true;
}
bool ensureAccessToken() {
  time_t now=time(nullptr);
  if (TOK.access.length()==0) loadTokens();
  if (TOK.access.length()==0) return doLogin();
  if (now>1600000000 && TOK.accessExp>0 && (TOK.accessExp-now)<60) {
    Serial.println("🔄 Access token expiring → refresh");
    if (doRefresh()) return true; return doLogin();
  }
  return true;
}

// ================== SYNC ALLOWED LIST ==================
void syncAllowedList() {
  if (WiFi.status()!=WL_CONNECTED) return;
  if (!ensureAccessToken()) { loadLocalAllowed(); return; }
  HTTPClient http; String url=String(SERVER_VALIDATE_BASE)+"/sync?device_id="+deviceId;
  if (!httpBeginSecure(http, url)) return;
  http.addHeader("Authorization","Bearer "+TOK.access);
  int code=http.GET();
  if (code==401) {
    http.end(); if (!doRefresh()) { loadLocalAllowed(); return; }
    if (!httpBeginSecure(http, url)) return; http.addHeader("Authorization","Bearer "+TOK.access); code=http.GET();
  }
  if (code!=200){ Serial.printf("Sync failed code=%d\n",code); http.end(); loadLocalAllowed(); return; }
  String body=http.getString(); http.end();
  if (deserializeJson(allowedList, body)!=DeserializationError::Ok){ Serial.println("Sync parse fail"); loadLocalAllowed(); return; }
  saveAllowedList(); Serial.printf("✅ Sync OK (%d items)\n", allowedList.size());
}

// ================== VALIDATION ==================
bool isAllowedLocal(const String &code) {
  if (!allowedList.is<JsonArray>()) return false;
  for (JsonVariant v : allowedList.as<JsonArray>()) if (code.equalsIgnoreCase(v.as<const char*>()))
    return true;
  return false;
}
bool isAllowedServer(const String &code) {
  if (WiFi.status()!=WL_CONNECTED) return true; // fallback allow (local akan menentukan)
  if (!ensureAccessToken()) return true;
  HTTPClient http; if (!httpBeginSecure(http, SERVER_VALIDATE_BASE)) return true;
  http.addHeader("Content-Type","application/json"); http.addHeader("Authorization","Bearer "+TOK.access);
  DynamicJsonDocument d(256); d["qr_data"]=code; d["device_id"]=deviceId; String body; serializeJson(d, body);
  int codeRes=http.POST(body); (void)codeRes;
  String resp=http.getString(); http.end();
  if (resp.indexOf("\"allow\":true")>=0) return true; if (resp.indexOf("\"allow\":false")>=0) return false; return true;
}

// ================== LOGGING ==================
void saveLogBuffer(){ File f=SPIFFS.open("/log_buffer.json","w"); if(!f){Serial.println("Save log_buffer failed"); return;} serializeJson(logBuffer,f); f.close(); }
void loadLogBuffer(){ if(!SPIFFS.exists("/log_buffer.json")){ logBuffer.clear(); logBuffer.to<JsonArray>(); Serial.println("✅ No log_buffer.json (new)"); return;} File f=SPIFFS.open("/log_buffer.json","r"); if(!f){ logBuffer.clear(); logBuffer.to<JsonArray>(); return;} if(deserializeJson(logBuffer,f)!=DeserializationError::Ok){ logBuffer.clear(); logBuffer.to<JsonArray>(); } f.close(); }
void addToLogBuffer(const String &qr, const String &status, const String &ts) {
  JsonArray arr=logBuffer.to<JsonArray>(); JsonObject obj=arr.createNestedObject();
  obj["device_id"]=deviceId; obj["qr_data"]=qr; obj["status"]=status; obj["timestamp"]=ts; saveLogBuffer();
  Serial.printf("Buffered log: %s %s\n", qr.c_str(), status.c_str());
}
String iso8601Now(){ time_t now=time(nullptr); if(now<1600000000) return String(millis()); struct tm t; localtime_r(&now,&t); char buf[32]; strftime(buf,sizeof(buf),"%Y-%m-%dT%H:%M:%S%z",&t); String s(buf); return s.substring(0,s.length()-2)+":"+s.substring(s.length()-2); }
void pushOfflineLogs() {
  if (WiFi.status()!=WL_CONNECTED || logBuffer.size()==0 || !ensureAccessToken()) return;
  HTTPClient http; String url=String(SERVER_VALIDATE_BASE)+"/log"; if(!httpBeginSecure(http,url)) return;
  http.addHeader("Content-Type","application/json"); http.addHeader("Authorization","Bearer "+TOK.access);
  for (int i=0;i<logBuffer.size();++i){ String j; serializeJson(logBuffer[i], j); int code=http.POST(j); if(code<200||code>=300){ Serial.printf("Push log failed idx=%d code=%d\n",i,code); http.end(); return;} delay(50); }
  logBuffer.clear(); saveLogBuffer(); http.end(); Serial.println("All offline logs pushed");
}
void sendLog(const String &qr, const String &status) {
  String ts=iso8601Now();
  if (WiFi.status()==WL_CONNECTED) {
    if (!ensureAccessToken()) { addToLogBuffer(qr,status,ts); return; }
    HTTPClient http; String url=String(SERVER_VALIDATE_BASE)+"/log"; if(!httpBeginSecure(http,url)){ addToLogBuffer(qr,status,ts); return; }
    http.addHeader("Content-Type","application/json"); http.addHeader("Authorization","Bearer "+TOK.access);
    DynamicJsonDocument d(256); d["device_id"]=deviceId; d["qr_data"]=qr; d["status"]=status; d["timestamp"]=ts; String json; serializeJson(d,json);
    int code=http.POST(json); http.end();
    if (code>=200 && code<300) return;
    if (code==401 && doRefresh()) { if(!httpBeginSecure(http,url)){ addToLogBuffer(qr,status,ts); return;} http.addHeader("Content-Type","application/json"); http.addHeader("Authorization","Bearer "+TOK.access); int code2=http.POST(json); http.end(); if(code2>=200&&code2<300) return; }
  }
  addToLogBuffer(qr,status,ts);
}

// ================== DOOR CONTROL ==================
void unlockDoor(){ digitalWrite(RELAY_PIN, LOW); doorUnlocked=true; unlockStart=millis(); setLedMode(LED_RGB_FLASH_OPEN_CYAN); Serial.println("Door unlocked"); }
void lockDoor(){ digitalWrite(RELAY_PIN, HIGH); doorUnlocked=false; setLedMode(WiFi.status()==WL_CONNECTED?LED_RGB_ON_GREEN:LED_RGB_BLINK_SLOW_GREEN); Serial.println("Door locked"); }

// ================== OTA VIA BACKEND (backend-triggered) ==================
bool startOTAFromURL(const String &url, const String &md5) {
  Serial.printf("⚡ OTA: Download from %s\n", url.c_str());

  HTTPClient http;
  bool okBegin = false;
  if (url.startsWith("https://")) {
    WiFiClientSecure client;
#if USE_INSECURE_TLS
    client.setInsecure();
#else
    client.setCACert(ROOT_CA);
#endif
    client.setTimeout(15000);
    okBegin = http.begin(client, url);
  } else {
    WiFiClient client;
    okBegin = http.begin(client, url);
  }
  if (!okBegin) { Serial.println("❌ OTA: failed to connect"); return false; }

  int code = http.GET();
  if (code != 200) { Serial.printf("❌ OTA: HTTP %d\n", code); http.end(); return false; }

  int len = http.getSize();
  WiFiClient * stream = http.getStreamPtr();

  if (md5.length() > 0) Update.setMD5(md5.c_str());

  setLedMode(LED_RGB_BLINK_FAST_BLUE); // indikasi OTA
  Serial.printf("⚡ OTA: size %d bytes\n", len);
  if (!Update.begin(len)) { Serial.println("❌ OTA: not enough space"); http.end(); setLedMode(LED_RGB_BLINK_FAST_RED); return false; }

  uint8_t buff[1024];
  int written = 0;
  unsigned long lastTick = millis();

  while (http.connected() && len > 0) {
    size_t sz = stream->available();
    if (sz > 0) {
      int c = stream->readBytes(buff, (sz > sizeof(buff)) ? sizeof(buff) : sz);
      if (c > 0) { Update.write(buff, c); written += c; len -= c; }
    }
    // kedip cepat biru saat OTA
    applyLedRGB();
    if (millis() - lastTick > 5000) { Serial.printf("OTA progress: %d bytes\r\n", written); lastTick = millis(); }
    delay(1);
  }
  http.end();

  if (!Update.end()) { Serial.printf("❌ OTA: %s\n", Update.errorString()); setLedMode(LED_RGB_BLINK_FAST_RED); return false; }
  if (!Update.isFinished()) { Serial.println("❌ OTA: not finished"); setLedMode(LED_RGB_BLINK_FAST_RED); return false; }

  Serial.printf("✅ OTA: done, wrote %d bytes. Rebooting...\n", written);
  delay(800); ESP.restart();
  return true;
}

// ================== HEARTBEAT & COMMAND HANDLER ==================
void handleBackendCommand(const String &resp) {
  DynamicJsonDocument d(1024);
  if (deserializeJson(d, resp)!=DeserializationError::Ok) { Serial.println("Heartbeat JSON parse error"); return; }

  String command = d["command"] | "NONE";
  if (command == "NONE") return;

  if (command == "UPDATE") {
    String url = d["url"].as<String>();
    String md5 = d["md5"].as<String>();
    Serial.println("⚡ Backend: OTA UPDATE");
    startOTAFromURL(url, md5);
  } else if (command == "RESTART") {
    Serial.println("⚡ Backend: RESTART");
    delay(300); ESP.restart();
  } else if (command == "SYNC") {
    Serial.println("⚡ Backend: SYNC allowed list");
    syncAllowedList();
  } else if (command == "WIPE_LOG") {
    Serial.println("⚡ Backend: WIPE LOG");
    logBuffer.clear(); saveLogBuffer();
  } else if (command == "WIPE_CRED") {
    Serial.println("⚡ Backend: WIPE CREDENT");
    allowedList.clear(); saveAllowedList();
  }
}

void sendHeartbeat() {
  if (WiFi.status()!=WL_CONNECTED) return;
  if (!ensureAccessToken()) return;

  HTTPClient http; if (!httpBeginSecure(http, HEARTBEAT_URL)) return;
  http.addHeader("Content-Type","application/json");
  http.addHeader("Authorization","Bearer "+TOK.access);

  DynamicJsonDocument d(1024);
  d["device_id"] = deviceId;
  d["firmware"]  = FIRMWARE_VERSION;
  d["wifi"]      = WiFi.RSSI();
  d["ip"]        = WiFi.localIP().toString();
  d["uptime"]    = millis();
  d["heap"]      = ESP.getFreeHeap();
  d["relay"]     = doorUnlocked ? "OPEN" : "LOCKED";
  d["last_qr"]   = lastQrData;
  d["connected"] = (WiFi.status()==WL_CONNECTED);
  // opsional, tambahkan metrik lain:
  // d["heap_min"]  = ESP.getMinFreeHeap();
  // d["psram_free"]= ESP.getFreePsram();
  // d["psram_min"] = ESP.getMinFreePsram();

  String json; serializeJson(d, json);
  int code = http.POST(json);
  if (code == 200) { String resp = http.getString(); handleBackendCommand(resp); }
  http.end();
}

// ================== OTA LOKAL (ArduinoOTA) ==================
void setupOTA() {
  ArduinoOTA.setHostname(deviceId);
  ArduinoOTA
    .onStart([](){ Serial.println("OTA(local) Start"); setLedMode(LED_RGB_BLINK_FAST_BLUE); })
    .onEnd([](){ Serial.println("OTA(local) End"); setLedMode(LED_RGB_ON_GREEN); })
    .onError([](ota_error_t e){ Serial.printf("OTA(local) Error[%u]\n",e); setLedMode(LED_RGB_BLINK_FAST_RED); });
  ArduinoOTA.begin();
  Serial.println("✅ OTA(local) ready");
}

// ================== NTP HELPER ==================
bool ntpSync(int maxAttempts = 20) {
  configTime(TZ_OFFSET, DST_OFFSET, "pool.ntp.org", "time.google.com", "id.pool.ntp.org");
  for (int i = 0; i < maxAttempts; ++i) {
    time_t now = time(nullptr);
    if (now > 1600000000) return true;
    delay(300);
  }
  return false;
}

// ================== SETUP ==================
void setup() {
  delay(300);                   // kecilkan gangguan USB CDC vs WS2812
  Serial.begin(115200);
  delay(300);

  // RGB init
  rgb.begin();
  rgb.setBrightness(50);
  rgbOff();
  setLedMode(LED_RGB_ON_BLUE);  // booting

  // IO
  QRSerial.begin(9600, SERIAL_8N1, QR_RX, QR_TX);
  pinMode(RELAY_PIN, OUTPUT); 
  digitalWrite(RELAY_PIN, HIGH); // locked

  // ID & storage
  generateDeviceId(); Serial.printf("Device ID: %s\n", deviceId);
  safeSPIFFS(); loadWifiConfig(); loadLogBuffer(); loadTokens(); loadLocalAllowed();

  if (!wifiConfigValid()) { setLedMode(LED_RGB_BLINK_FAST_RED); startConfigPortal(true); return; }

  // WiFi connect w/ retries
  WiFi.mode(WIFI_STA); bool ok=false;
  for (int attempt=1; attempt<=3; attempt++){
    Serial.printf("WiFi connect attempt %d...\n", attempt);
    WiFi.begin(wifiCfg.ssid.c_str(), wifiCfg.password.c_str());
    const unsigned long t0=millis();
    setLedMode(LED_RGB_BLINK_SLOW_GREEN); // mencari WiFi
    while (WiFi.status()!=WL_CONNECTED && millis()-t0<20000) { applyLedRGB(); delay(100); }
    if (WiFi.status()==WL_CONNECTED){ ok=true; break; }
  }
  if (!ok){ Serial.println("WiFi failed → portal"); startConfigPortal(true); return; }

  wifiAvailable=true; wifiDownSince=0;
  Serial.printf("✅ WiFi connected: %s\n", WiFi.localIP().toString().c_str());
  setLedMode(LED_RGB_ON_GREEN);

  // NTP + auth + initial sync/log push
  if (ntpSync()) Serial.println("NTP sync OK"); else Serial.println("NTP sync failed (use millis)");
  if (!ensureAccessToken()) Serial.println("Auth failed (fallback local)");
  syncAllowedList(); pushOfflineLogs();

  // OTA lokal aktif (opsional)
  setupOTA();
}

// ================== LOOP ==================
void loop() {
  // LED scheduler
  applyLedRGB();

  // OTA lokal handler
  ArduinoOTA.handle();

  // Auto lock
  if (doorUnlocked && (millis()-unlockStart >= UNLOCK_MS)) lockDoor();

  // WiFi auto-reconnect + fail-safe portal
  if (WiFi.status()!=WL_CONNECTED) {
    if (wifiDownSince==0) wifiDownSince=millis();
    setLedMode(LED_RGB_BLINK_SLOW_GREEN);
    WiFi.reconnect(); delay(500);
    if (WiFi.status()==WL_CONNECTED) {
      wifiAvailable=true; wifiDownSince=0; Serial.println("✅ WiFi Reconnected"); setLedMode(LED_RGB_ON_GREEN);
    } else {
      wifiAvailable=false;
      if (wifiDownSince && (millis()-wifiDownSince >= WIFI_FAIL_PORTAL_AFTER)) {
        Serial.println("WiFi down too long → portal");
        startConfigPortal(true); return;
      }
    }
  }

  // periodic jobs
  if (wifiAvailable && (millis()-lastSync >= SYNC_INTERVAL_MS)) { syncAllowedList(); lastSync=millis(); }
  if (wifiAvailable && (millis()-lastPush >= PUSH_INTERVAL_MS)) { pushOfflineLogs(); lastPush=millis(); }
  if (wifiAvailable && (millis()-lastHeartbeat >= HEARTBEAT_INTERVAL_MS)) { lastHeartbeat=millis(); sendHeartbeat(); }

  // QR read (debounce + anti-dupe)
  if (QRSerial.available()) {
    String qr = QRSerial.readStringUntil('\n');
    qr.trim();
    if (qr.length()==0) return;

    const unsigned long now=millis();
    if (qr==lastQrData && (now-lastQrTs)<QR_DEBOUNCE_MS) return; // ignore burst
    lastQrData=qr; lastQrTs=now;

    Serial.printf("QR read: %s\n", qr.c_str());
    bool localOk = isAllowedLocal(qr);
    bool serverOk = isAllowedServer(qr);

    if (localOk && serverOk) { unlockDoor(); sendLog(qr,"OPEN"); }
    else { Serial.println("Access DENIED"); setLedMode(LED_RGB_BLINK_FAST_RED); sendLog(qr,"DENIED"); }
  }
}
