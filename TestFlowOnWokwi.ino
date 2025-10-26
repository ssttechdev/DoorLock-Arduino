#include <Arduino.h>

// pins
#define RELAY_PIN 26
#define UNLOCK_MS 5000UL

// device id
char deviceId[32];

// allowed list (local) - initially empty; will be filled on "sync"
#include <vector>
#include <string>
std::vector<String> allowedList;

// offline log buffer (simple in-memory vector of JSON-like strings)
std::vector<String> logBuffer;

// state
bool wifiOnline = false;
bool doorUnlocked = false;
unsigned long unlockStart = 0;

// helper: generate device id (simulated like ESP.getEfuseMac)
void generateDeviceId() {
  unsigned long a = (unsigned long)esp_rand();
  unsigned long b = (unsigned long)esp_random();
  sprintf(deviceId, "MYPR-%08lX%08lX", a, b);
}

// helpers for allowed list
void setAllowedFromServerMock() {
  // server-sim: return a fixed set
  allowedList.clear();
  allowedList.push_back("EMP001");
  allowedList.push_back("EMP002");
  allowedList.push_back("GUEST123");
  Serial.printf("Mock server: provided %d allowed codes\n", (int)allowedList.size());
}

bool isAllowedLocal(const String &code) {
  for (auto &s : allowedList) if (s.equalsIgnoreCase(code)) return true;
  return false;
}

// log buffer ops
void pushLogBuffer(const String &qr, const String &status) {
  String ts = String(millis());
  String rec = "{\"device_id\":\"" + String(deviceId) + "\",\"qr_data\":\"" + qr + "\",\"status\":\"" + status + "\",\"timestamp\":\"" + ts + "\"}";
  logBuffer.push_back(rec);
  Serial.printf("Buffered log (now %d): %s\n", (int)logBuffer.size(), rec.c_str());
}

void pushOfflineLogsMock() {
  if (!wifiOnline) return;
  if (logBuffer.empty()) {
    Serial.println("No offline logs to push.");
    return;
  }
  Serial.printf("Pushing %d buffered logs to mock server...\n", (int)logBuffer.size());
  for (size_t i=0;i<logBuffer.size();++i) {
    Serial.printf(" -> pushed: %s\n", logBuffer[i].c_str());
    delay(50);
  }
  logBuffer.clear();
  Serial.println("All buffered logs pushed (mock).");
}

// mock server validate: simulate an online check (always returns same as allowedList)
bool isAllowedServerMock(const String &code) {
  // in mock, simply check allowedList (server and local same)
  return isAllowedLocal(code);
}

// send realtime log (mock)
void sendLogMock(const String &qr, const String &status) {
  if (!wifiOnline) {
    pushLogBuffer(qr, status);
    return;
  }
  String ts = String(millis());
  String rec = "{\"device_id\":\"" + String(deviceId) + "\",\"qr_data\":\"" + qr + "\",\"status\":\"" + status + "\",\"timestamp\":\"" + ts + "\"}";
  Serial.printf("Sent realtime log to mock server: %s\n", rec.c_str());
}

// door control
void unlockDoor() {
  digitalWrite(RELAY_PIN, LOW); // active LOW
  doorUnlocked = true;
  unlockStart = millis();
  Serial.println("** DOOR UNLOCKED (simulated) **");
}

void lockDoor() {
  digitalWrite(RELAY_PIN, HIGH);
  doorUnlocked = false;
  Serial.println("** DOOR LOCKED (simulated) **");
}

// process a scanned QR
void handleScan(const String &code) {
  Serial.printf("QR scanned: %s\n", code.c_str());
  bool localOk = isAllowedLocal(code);
  bool serverOk = isAllowedServerMock(code); // mock server call
  Serial.printf(" local=%s server=%s\n", localOk ? "OK" : "NO", serverOk ? "OK" : "NO");
  if (localOk && serverOk) {
    unlockDoor();
    sendLogMock(code, "OPEN");
  } else {
    Serial.println("ACCESS DENIED");
    sendLogMock(code, "DENIED");
  }
}

// parse serial command
void handleCommand(const String &line) {
  String s = line;
  s.trim();
  if (s.length() == 0) return;
  if (s.equalsIgnoreCase("help")) {
    Serial.println("Commands: help, wifi on, wifi off, scan <code>, push UNLOCK, push LOCK, status");
    return;
  }
  if (s.equalsIgnoreCase("wifi on")) {
    wifiOnline = true;
    Serial.println("Simulate WiFi: ON");
    // simulate sync
    setAllowedFromServerMock();
    pushOfflineLogsMock();
    return;
  }
  if (s.equalsIgnoreCase("wifi off")) {
    wifiOnline = false;
    Serial.println("Simulate WiFi: OFF");
    return;
  }
  if (s.startsWith("scan ")) {
    String code = s.substring(5);
    code.trim();
    handleScan(code);
    return;
  }
  if (s.startsWith("push ")) {
    String cmd = s.substring(5);
    cmd.trim();
    Serial.printf("Mock server push: %s\n", cmd.c_str());
    if (cmd.equalsIgnoreCase("UNLOCK")) {
      unlockDoor();
    } else if (cmd.equalsIgnoreCase("LOCK")) {
      lockDoor();
    } else if (cmd.equalsIgnoreCase("SYNC_NOW")) {
      setAllowedFromServerMock();
    } else {
      Serial.println("Unknown push command");
    }
    return;
  }
  if (s.equalsIgnoreCase("status")) {
    Serial.printf("Device: %s\n", deviceId);
    Serial.printf("WiFi: %s\n", wifiOnline ? "ONLINE" : "OFFLINE");
    Serial.printf("Allowed list (%d): ", (int)allowedList.size());
    for (auto &a : allowedList) Serial.printf("%s ", a.c_str());
    Serial.println();
    Serial.printf("Buffered logs: %d\n", (int)logBuffer.size());
    return;
  }
  Serial.println("Unknown command. Type 'help'.");
}

void setup() {
  Serial.begin(115200);
  delay(200);
  pinMode(RELAY_PIN, OUTPUT);
  digitalWrite(RELAY_PIN, HIGH); // locked by default

  // generate device id
  uint32_t r = (uint32_t)esp_random();
  sprintf(deviceId, "MYPR-%08X", r);
  Serial.printf("Simulated Device ID: %s\n", deviceId);
  Serial.println("Type 'help' for commands. Example: 'wifi on' then 'scan EMP001'");

  // initial local allowed list empty (simulate offline start)
  allowedList.clear();
  logBuffer.clear();
}

String inputBuffer = "";
void loop() {
  // auto-lock timeout
  if (doorUnlocked && (millis() - unlockStart >= UNLOCK_MS)) {
    lockDoor();
  }

  // read serial line-by-line
  while (Serial.available() > 0) {
    char c = Serial.read();
    if (c == '\r') continue;
    if (c == '\n') {
      String line = inputBuffer;
      inputBuffer = "";
      handleCommand(line);
    } else {
      inputBuffer += c;
      // prevent runaway
      if (inputBuffer.length() > 200) inputBuffer = inputBuffer.substring(inputBuffer.length()-200);
    }
  }
}
