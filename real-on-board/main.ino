#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <FS.h>
#include <SPIFFS.h>
#include <ArduinoJson.h>
#include <time.h>

#define WIFI_SSID      "SSID"
#define WIFI_PASS      "PASS"

#define SERVER_BASE    "https://project.sstech.id/IOT/api/validate"
#define AUTH_LOGIN_URL "https://project.sstech.id/IOT/api/auth/login"
#define AUTH_REFRESH_URL "https://project.sstech.id/IOT/api/auth/refresh"

#define DEVICE_PSK     "super-secret-device-psk"

// CA chain PEM (full chain/ intermediate)
static const char* ROOT_CA = R"(-----BEGIN CERTIFICATE-----
MIIF...REPLACE_WITH_YOUR_CA_CHAIN...== 
-----END CERTIFICATE-----
)";

// pins & hardware
#define RELAY_PIN 26
#define QR_RX     16  // RX2 (terima dari QR module TX)
#define QR_TX     17  // TX2 (opsional)
#define UNLOCK_MS 5000UL

// timing
#define SYNC_INTERVAL_MS (10UL * 60UL * 1000UL) // 10 menit
#define PUSH_INTERVAL_MS (30UL * 1000UL)        // 30 detik
#define LOGPULL_INTERVAL_MS (60UL * 60UL * 1000UL) // 1 jam

// NTP (WIB)
#define TZ_OFFSET (7 * 3600)
#define DST_OFFSET 0

// ===== GLOBALS =====
HardwareSerial QRSerial(2);
char deviceId[32];
bool wifiAvailable = false;
bool doorUnlocked = false;
unsigned long unlockStart = 0;

// JSON docs
DynamicJsonDocument allowedList(4096);
DynamicJsonDocument logBuffer(8192); // array of logs

// Auth token struct
struct Tokens {
  String access;
  String refresh;
  long accessExp;   // epoch seconds
  long refreshExp;  // epoch seconds (optional)
} TOK;

// last timestamps
unsigned long lastSync = 0;
unsigned long lastPush = 0;
unsigned long lastLogPull = 0;

// ===== Helper: Secure HTTP begin with CA pinning =====
bool httpBeginSecure(HTTPClient &http, const String &url) {
  static WiFiClientSecure *client = nullptr;
  if (!client) {
    client = new WiFiClientSecure();
    client->setCACert(ROOT_CA);
    client->setTimeout(15000);
  }
  return http.begin(*client, url);
}

// ===== Device ID from Efuse MAC =====
void generateDeviceId() {
  uint64_t chipid = ESP.getEfuseMac();
  sprintf(deviceId, "MYRP-%04X%08X", (uint16_t)(chipid >> 32), (uint32_t)chipid);
}

// ===== SPIFFS file ops: allowed.json =====
void saveAllowedList() {
  File f = SPIFFS.open("/credent.json", "w");
  if (!f) { Serial.println("Save creden failed"); return; }
  serializeJson(allowedList, f);
  f.close();
  Serial.println("Saved creden");
}

void loadLocalAllowed() {
  if (!SPIFFS.exists("/credent.json")) {
    Serial.println("creden not found; local list empty");
    allowedList.clear();
    return;
  }
  File f = SPIFFS.open("/credent.json", "r");
  if (!f) { Serial.println("Open credent failed"); return; }
  DeserializationError err = deserializeJson(allowedList, f);
  f.close();
  if (err) {
    Serial.println("deserialize creden failed");
    allowedList.clear();
  } else {
    Serial.printf("Loaded creden (%d items)\n", allowedList.size());
  }
}

// ===== SPIFFS file ops: log_buffer.json =====
void saveLogBuffer() {
  File f = SPIFFS.open("/log_buffer.json", "w");
  if (!f) { Serial.println("Save log_buffer failed"); return; }
  serializeJson(logBuffer, f);
  f.close();
}

void loadLogBuffer() {
  if (!SPIFFS.exists("/log_buffer.json")) {
    logBuffer.clear();
    logBuffer.to<JsonArray>();
    Serial.println("No log_buffer.json (start empty)");
    return;
  }
  File f = SPIFFS.open("/log_buffer.json", "r");
  if (!f) { logBuffer.clear(); logBuffer.to<JsonArray>(); return; }
  DeserializationError err = deserializeJson(logBuffer, f);
  f.close();
  if (err) {
    Serial.println("deserialize log_buffer failed; clear");
    logBuffer.clear();
    logBuffer.to<JsonArray>();
  } else {
    Serial.printf("Loaded log buffer (%d items)\n", logBuffer.size());
  }
}

// ===== Auth tokens save/load =====
void saveTokens() {
  DynamicJsonDocument d(1024);
  d["access"] = TOK.access;
  d["refresh"] = TOK.refresh;
  d["accessExp"] = TOK.accessExp;
  d["refreshExp"] = TOK.refreshExp;
  File f = SPIFFS.open("/tokens.json", "w");
  if (!f) { Serial.println("save tokens failed"); return; }
  serializeJson(d, f);
  f.close();
}

void loadTokens() {
  if (!SPIFFS.exists("/tokens.json")) return;
  File f = SPIFFS.open("/tokens.json", "r");
  if (!f) return;
  DynamicJsonDocument d(1024);
  if (deserializeJson(d, f) == DeserializationError::Ok) {
    TOK.access = d["access"] | "";
    TOK.refresh = d["refresh"] | "";
    TOK.accessExp = d["accessExp"] | 0;
    TOK.refreshExp = d["refreshExp"] | 0;
    Serial.println("Loaded tokens from Local");
  }
  f.close();
}

// ===== NTP & time helpers =====
bool ntpSync(int maxAttempts = 20) {
  configTime(TZ_OFFSET, DST_OFFSET, "pool.ntp.org", "time.google.com", "id.pool.ntp.org");
  for (int i = 0; i < maxAttempts; ++i) {
    time_t now = time(nullptr);
    if (now > 1600000000) return true;
    delay(300);
  }
  return false;
}

String iso8601Now() {
  time_t now = time(nullptr);
  if (now < 1600000000) { // time not set
    // fallback to millis as string
    return String(millis());
  }
  struct tm t;
  localtime_r(&now, &t);
  char buf[32];
  strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%S%z", &t);
  // insert colon in timezone offset for ISO8601: +0700 -> +07:00
  String s(buf);
  if (s.length() >= 5) s = s.substring(0, s.length()-2) + ":" + s.substring(s.length()-2);
  return s;
}

// ===== Auth flows: login & refresh (server must return access_exp) =====
bool doLogin() {
  if (WiFi.status() != WL_CONNECTED) return false;
  HTTPClient http;
  String url = String(AUTH_LOGIN_URL);
  if (!httpBeginSecure(http, url)) return false;
  http.addHeader("Content-Type", "application/json");
  // request body: device_id + psk
  String body;
  {
    DynamicJsonDocument d(256);
    d["device_id"] = deviceId;
    d["psk"] = DEVICE_PSK;
    serializeJson(d, body);
  }
  int code = http.POST(body);
  if (code < 200 || code >= 300) {
    Serial.printf("Login failed code=%d\n", code);
    http.end();
    return false;
  }
  String resp = http.getString();
  http.end();
  DynamicJsonDocument d(1024);
  if (deserializeJson(d, resp) != DeserializationError::Ok) {
    Serial.println("Login: parse resp failed");
    return false;
  }
  TOK.access = String((const char*)d["access_token"] | "");
  TOK.refresh = String((const char*)d["refresh_token"] | "");
  TOK.accessExp = (long)(d["access_exp"] | 0);
  TOK.refreshExp = (long)(d["refresh_exp"] | 0);
  saveTokens();
  Serial.println("Login OK, tokens saved");
  return true;
}

bool doRefresh() {
  if (WiFi.status() != WL_CONNECTED) return false;
  if (TOK.refresh.length() == 0) return false;
  HTTPClient http;
  String url = String(AUTH_REFRESH_URL);
  if (!httpBeginSecure(http, url)) return false;
  http.addHeader("Content-Type", "application/json");
  String body;
  {
    DynamicJsonDocument d(256);
    d["refresh_token"] = TOK.refresh;
    serializeJson(d, body);
  }
  int code = http.POST(body);
  if (code < 200 || code >= 300) {
    Serial.printf("Refresh failed code=%d\n", code);
    http.end();
    return false;
  }
  String resp = http.getString();
  http.end();
  DynamicJsonDocument d(1024);
  if (deserializeJson(d, resp) != DeserializationError::Ok) {
    Serial.println("Refresh: parse resp failed");
    return false;
  }
  TOK.access = String((const char*)d["access_token"] | "");
  TOK.accessExp = (long)(d["access_exp"] | 0);
  if (d.containsKey("refresh_token")) {
    TOK.refresh = String((const char*)d["refresh_token"] | "");
    TOK.refreshExp = (long)(d["refresh_exp"] | 0);
  }
  saveTokens();
  Serial.println("Refresh OK");
  return true;
}

bool ensureAccessToken() {
  time_t now = time(nullptr);
  if (TOK.access.length() == 0) {
    loadTokens();
  }
  if (TOK.access.length() == 0) {
    // no token -> login
    return doLogin();
  }
  // if time available, check expiry
  if (now > 1600000000 && TOK.accessExp > 0) {
    if ((TOK.accessExp - now) < 60) { // expire within 60s
      Serial.println("Access token near expiry -> refresh");
      if (doRefresh()) return true;
      // refresh failed -> try login
      return doLogin();
    }
  }
  return true;
}

// ===== sync allowed list from server =====
void syncAllowedList() {
  if (WiFi.status() != WL_CONNECTED) return;
  if (!ensureAccessToken()) { loadLocalAllowed(); return; }

  HTTPClient http;
  String url = String(SERVER_BASE) + "/sync?device_id=" + deviceId;
  if (!httpBeginSecure(http, url)) return;
  http.addHeader("Authorization", "Bearer " + TOK.access);
  int code = http.GET();
  if (code == 401) { // try refresh
    http.end();
    if (!doRefresh()) { loadLocalAllowed(); return; }
    if (!httpBeginSecure(http, url)) return;
    http.addHeader("Authorization", "Bearer " + TOK.access);
    code = http.GET();
  }
  if (code != 200) {
    Serial.printf("Sync failed code=%d\n", code);
    http.end();
    loadLocalAllowed();
    return;
  }
  String body = http.getString();
  http.end();
  DeserializationError err = deserializeJson(allowedList, body);
  if (err) {
    Serial.println("Sync: parse allowed failed");
    loadLocalAllowed();
    return;
  }
  saveAllowedList();
  Serial.printf("Sync OK (%d items)\n", allowedList.size());
}

// ===== local check =====
bool isAllowedLocal(const String &code) {
  if (allowedList.isNull() || allowedList.size() == 0) return false;
  for (JsonVariant v : allowedList.as<JsonArray>()) {
    if (code.equalsIgnoreCase(v.as<const char*>())) return true;
  }
  return false;
}

// ===== server validate =====
bool isAllowedServer(const String &code) {
  if (WiFi.status() != WL_CONNECTED) return true; // fallback allow check? we choose fallback true so local controls actual allow
  if (!ensureAccessToken()) return true;
  HTTPClient http;
  String url = String(SERVER_BASE);
  if (!httpBeginSecure(http, url)) return true;
  http.addHeader("Content-Type", "application/json");
  http.addHeader("Authorization", "Bearer " + TOK.access);
  String body;
  { DynamicJsonDocument d(256); d["qr_data"] = code; d["device_id"] = deviceId; serializeJson(d, body); }
  int codeRes = http.POST(body);
  if (codeRes == 401) {
    http.end();
    if (!doRefresh()) return true;
    if (!httpBeginSecure(http, url)) return true;
    http.addHeader("Content-Type", "application/json");
    http.addHeader("Authorization", "Bearer " + TOK.access);
    codeRes = http.POST(body);
  }
  if (codeRes <= 0) { http.end(); return true; } // fallback true
  String resp = http.getString();
  http.end();
  if (resp.indexOf("\"allow\":true") >= 0) return true;
  if (resp.indexOf("\"allow\":false") >= 0) return false;
  return true;
}

// ===== log buffer helpers =====
void addToLogBuffer(const String &qr, const String &status, const String &ts) {
  JsonArray arr = logBuffer.to<JsonArray>();
  if (!arr) {
    logBuffer.clear();
    arr = logBuffer.to<JsonArray>();
  }
  JsonObject obj = arr.createNestedObject();
  obj["device_id"] = deviceId;
  obj["qr_data"] = qr;
  obj["status"] = status;
  obj["timestamp"] = ts;
  saveLogBuffer();
  Serial.printf("Buffered log: %s %s\n", qr.c_str(), status.c_str());
}

// push offline logs one-by-one (robust)
void pushOfflineLogs() {
  if (WiFi.status() != WL_CONNECTED) return;
  if (logBuffer.size() == 0) return;
  if (!ensureAccessToken()) return;

  JsonArray arr = logBuffer.as<JsonArray>();
  int total = arr.size();
  Serial.printf("Pushing %d offline logs\n", total);

  HTTPClient http;
  String url = String(SERVER_BASE) + "/log";
  if (!httpBeginSecure(http, url)) return;
  http.addHeader("Content-Type", "application/json");
  http.addHeader("Authorization", "Bearer " + TOK.access);

  // send one by one
  for (int i = 0; i < total; ++i) {
    String json;
    serializeJson(arr[i], json);
    int code = http.POST(json);
    if (code < 200 || code >= 300) {
      Serial.printf("Push log failed idx=%d code=%d\n", i, code);
      http.end();
      return; // abort; retry later
    } else {
      Serial.printf("Pushed offline %d/%d\n", i+1, total);
    }
    delay(50);
  }
  // all sent -> clear buffer
  logBuffer.clear();
  saveLogBuffer();
  http.end();
  Serial.println("All offline logs pushed");
}

// ===== send log realtime (with fallback to buffer) =====
void sendLog(const String &qr, const String &status) {
  String ts = iso8601Now();
  if (WiFi.status() == WL_CONNECTED) {
    if (!ensureAccessToken()) { addToLogBuffer(qr, status, ts); return; }
    HTTPClient http;
    String url = String(SERVER_BASE) + "/log";
    if (!httpBeginSecure(http, url)) { addToLogBuffer(qr, status, ts); return; }
    http.addHeader("Content-Type", "application/json");
    http.addHeader("Authorization", "Bearer " + TOK.access);
    DynamicJsonDocument d(256);
    d["device_id"] = deviceId;
    d["qr_data"] = qr;
    d["status"] = status;
    d["timestamp"] = ts;
    String json; serializeJson(d, json);
    int code = http.POST(json);
    http.end();
    if (code >= 200 && code < 300) {
      Serial.println("Log sent realtime");
      return;
    } else if (code == 401) {
      // try refresh once
      if (doRefresh()) {
        // retry once
        if (!httpBeginSecure(http, url)) { addToLogBuffer(qr, status, ts); return; }
        http.addHeader("Content-Type", "application/json");
        http.addHeader("Authorization", "Bearer " + TOK.access);
        int code2 = http.POST(json);
        http.end();
        if (code2 >=200 && code2 < 300) { Serial.println("Log sent after refresh"); return; }
      }
    }
  }
  // fallback save offline
  addToLogBuffer(qr, status, ts);
}

// ===== Door control =====
void unlockDoor() {
  digitalWrite(RELAY_PIN, LOW); // active LOW assumed; adjust if needed
  doorUnlocked = true;
  unlockStart = millis();
  Serial.println("Door unlocked");
}
void lockDoor() {
  digitalWrite(RELAY_PIN, HIGH);
  doorUnlocked = false;
  Serial.println("Door locked");
}

// ===== Startup =====
void setup() {
  Serial.begin(115200);
  delay(100);

  // init UART for QR
  QRSerial.begin(9600, SERIAL_8N1, QR_RX, QR_TX);
  pinMode(RELAY_PIN, OUTPUT);
  digitalWrite(RELAY_PIN, HIGH); // locked

  // device id
  generateDeviceId();
  Serial.printf("Device ID: %s\n", deviceId);

  // SPIFFS
  if (!SPIFFS.begin(true)) Serial.println("SPIFFS mount failed");
  loadLogBuffer(); // load buffered logs
  loadTokens();    // load tokens if any
  loadLocalAllowed();

  // connect WiFi
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  Serial.print("Connecting WiFi");
  unsigned long start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < 10000) {
    Serial.print(".");
    delay(500);
  }
  if (WiFi.status() == WL_CONNECTED) {
    wifiAvailable = true;
    Serial.printf("\nWiFi connected: %s\n", WiFi.localIP().toString().c_str());
    // NTP
    if (ntpSync()) Serial.println("NTP sync OK");
    else Serial.println("NTP sync failed (will use millis)");
    // auth & sync
    if (!ensureAccessToken()) Serial.println("Auth failed (will fallback to local)");
    syncAllowedList();
    pushOfflineLogs();
  } else {
    wifiAvailable = false;
    Serial.println("\nWiFi not connected - offline mode");
  }
}

// ===== Loop =====
void loop() {
  // auto lock after timeout
  if (doorUnlocked && (millis() - unlockStart >= UNLOCK_MS)) {
    lockDoor();
  }

  // periodic tasks
  if (wifiAvailable && (millis() - lastSync >= SYNC_INTERVAL_MS)) {
    syncAllowedList();
    lastSync = millis();
  }
  if (wifiAvailable && (millis() - lastPush >= PUSH_INTERVAL_MS)) {
    pushOfflineLogs();
    lastPush = millis();
  }
  if (wifiAvailable && (millis() - lastLogPull >= LOGPULL_INTERVAL_MS)) {
    // optional: implement get logs from server if needed
    lastLogPull = millis();
  }

  // read QR scanner
  if (QRSerial.available()) {
    String qr = QRSerial.readStringUntil('\n');
    qr.trim();
    if (qr.length() == 0) return;
    Serial.printf("QR read: %s\n", qr.c_str());
    bool localOk = isAllowedLocal(qr);
    bool serverOk = isAllowedServer(qr);
    if (localOk && serverOk) {
      unlockDoor();
      sendLog(qr, "OPEN");
    } else {
      Serial.println("Access DENIED");
      sendLog(qr, "DENIED");
    }
  }
}
