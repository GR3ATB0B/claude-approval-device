/*
 * Claude Code Approval Device — pure-NUS firmware.
 *
 * Hardware: Seeed Studio XIAO ESP32-S3
 *
 * No HID. The device advertises the Nordic UART Service (NUS) only and emits
 * structured JSON over the TX characteristic. A macOS menu-bar middleman owns
 * the BLE bond, receives button/switch events as JSON, and injects keystrokes
 * into the focused window via Quartz Event Services (CGEventPost). Anthropic's
 * Claude Desktop Hardware Buddy panel also speaks this protocol natively.
 *
 * INPUT (silkscreen pins):
 *   D0 - Function button   -> emits {"evt":"button","id":"function","action":"press"|"release"}
 *   D1 - Return button     -> emits {"evt":"button","id":"return","action":"tap"}
 *   D2 - Auto-accept switch -> emits {"evt":"switch","id":"auto_accept","state":"on"|"off"}
 *
 * OUTPUT:
 *   D5 - passive buzzer (jingles + tones)
 *   D8 - blue LED (PWM, inverted: LOW = on)
 *
 * BLE SERVICE:
 *   Nordic UART  6E400001-B5A3-F393-E0A9-E50E24DCCA9E
 *   RX (write)   6E400002-...   host -> device, newline-delimited JSON
 *   TX (notify)  6E400003-...   device -> host, newline-delimited JSON
 *
 * USB SERIAL (115200): same JSON commands work over USB CDC for development.
 *
 * Requires: arduino-esp32 3.3.7+, NimBLE-Arduino 2.3.8+, ArduinoJson 7.x.
 */

#include <NimBLEDevice.h>
#include <ArduinoJson.h>

// ── Pin map (XIAO ESP32-S3 silkscreen) ───────────────────────────────────────

#define PIN_FUNC_BTN    D0
#define PIN_RETURN_BTN  D1
#define PIN_AUTO_SWITCH D2
#define PIN_BUZZER      D5
// Battery sense — XIAO ESP32-S3 v1.0 has an internal 200k+200k divider from
// BAT+ to D9 (GPIO8). Older revisions don't; wire an external 100k+100k
// divider from BAT+ to D9 to GND. Either way the ADC reads ~half battery V.
#define PIN_BATTERY     D9
#define PIN_LED         D8

// ── BLE NUS UUIDs ────────────────────────────────────────────────────────────

#define NUS_SERVICE_UUID "6E400001-B5A3-F393-E0A9-E50E24DCCA9E"
#define NUS_RX_UUID      "6E400002-B5A3-F393-E0A9-E50E24DCCA9E"  // host -> device
#define NUS_TX_UUID      "6E400003-B5A3-F393-E0A9-E50E24DCCA9E"  // device -> host

NimBLECharacteristic* nusRxChar = nullptr;
NimBLECharacteristic* nusTxChar = nullptr;
String nusRxBuffer;
volatile bool centralConnected = false;

// ── LED state ────────────────────────────────────────────────────────────────

enum LEDStatus { LED_OFF, LED_BREATHING, LED_PULSING, LED_SOLID, LED_FLASHING };
LEDStatus currentLEDStatus = LED_BREATHING;

unsigned long lastLEDUpdate = 0;
int ledBrightness = 0;
bool ledDirection = true;
bool ledState = false;

// ── Button / switch state ────────────────────────────────────────────────────

bool lastFuncState = HIGH;
bool lastReturnState = HIGH;
bool lastSwitchState = HIGH;
unsigned long lastFuncPress = 0;
unsigned long lastReturnPress = 0;
const unsigned long debounceDelay = 50;

// ── Hardware Buddy state (when Anthropic's panel or our middleman is connected) ──

String pendingPromptId;
unsigned long lastHeartbeatMs = 0;
int hbRunning = 0, hbWaiting = 0;
String deviceName = "Clawd";
String ownerName  = "";
uint32_t apprCount = 0, denyCount = 0;
unsigned long bootMs = 0;

// Battery sense
unsigned long lastBatteryEmit = 0;
const unsigned long batteryEmitInterval = 30000;  // 30 s

float readBatteryVoltage() {
  // ESP32 ADC is 12-bit (0..4095) referenced to ~3.3V. Divider is 2:1.
  int raw = analogRead(PIN_BATTERY);
  float v_pin = (raw / 4095.0f) * 3.3f;
  return v_pin * 2.0f;  // back-out divider
}

int readBatteryPercent() {
  float v = readBatteryVoltage();
  // Map 3.0V -> 0 %, 4.2V -> 100 %.
  float pct = (v - 3.0f) / (4.2f - 3.0f) * 100.0f;
  if (pct < 0) pct = 0;
  if (pct > 100) pct = 100;
  return (int)pct;
}

// USB serial command buffer (same JSON protocol over USB CDC)
String usbBuffer = "";

// ── BLE callback classes ─────────────────────────────────────────────────────

void processLine(const String& line);
void sendBuddy(const String& json);
void emitHello();
void playTone(int frequency, int duration);
void playJingle(String name);

class NusRxCallbacks : public NimBLECharacteristicCallbacks {
  void onWrite(NimBLECharacteristic* chr, NimBLEConnInfo& info) override {
    std::string val = chr->getValue();
    if (val.empty()) return;
    nusRxBuffer.concat(val.c_str());
    int nl;
    while ((nl = nusRxBuffer.indexOf('\n')) >= 0) {
      String line = nusRxBuffer.substring(0, nl);
      nusRxBuffer.remove(0, nl + 1);
      processLine(line);
    }
  }
};

class ServerCallbacks : public NimBLEServerCallbacks {
  void onConnect(NimBLEServer* server, NimBLEConnInfo& info) override {
    centralConnected = true;
    Serial.println("[BLE] central connected");
    // Send hello so the middleman knows who we are
    emitHello();
  }
  void onDisconnect(NimBLEServer* server, NimBLEConnInfo& info, int reason) override {
    centralConnected = false;
    Serial.printf("[BLE] central disconnected (reason=%d)\n", reason);
    NimBLEDevice::startAdvertising();
  }
};

NusRxCallbacks   nusCallbacks;
ServerCallbacks  serverCallbacks;

// ── BLE TX helpers ───────────────────────────────────────────────────────────

void sendBuddy(const String& json) {
  Serial.print("[TX] ");
  Serial.println(json);
  if (!nusTxChar || !centralConnected) return;
  String line = json + "\n";
  nusTxChar->setValue((uint8_t*)line.c_str(), line.length());
  nusTxChar->notify();
}

void emitHello() {
  JsonDocument d;
  d["evt"] = "hello";
  d["name"] = "ClaudeApprover";
  d["fw"] = "1.0";
  String s; serializeJson(d, s);
  sendBuddy(s);
}

void emitBatteryEvent() {
  JsonDocument d;
  d["evt"] = "battery";
  d["pct"] = readBatteryPercent();
  d["mV"] = (int)(readBatteryVoltage() * 1000.0f);
  String s; serializeJson(d, s);
  sendBuddy(s);
}

void emitButtonEvent(const char* id, const char* action) {
  JsonDocument d;
  d["evt"] = "button";
  d["id"] = id;
  d["action"] = action;
  String s; serializeJson(d, s);
  sendBuddy(s);
}

void emitSwitchEvent(const char* id, const char* state) {
  JsonDocument d;
  d["evt"] = "switch";
  d["id"] = id;
  d["state"] = state;
  String s; serializeJson(d, s);
  sendBuddy(s);
}

void respondPermission(const char* decision) {
  if (pendingPromptId.length() == 0) return;
  JsonDocument d;
  d["cmd"] = "permission";
  d["id"] = pendingPromptId;
  d["decision"] = decision;
  String s; serializeJson(d, s);
  sendBuddy(s);
  if (String(decision) == "once") { apprCount++; playJingle("approved"); }
  else                            { denyCount++; playJingle("denied"); }
  pendingPromptId = "";
  currentLEDStatus = LED_PULSING;
}

// ── Inbound JSON router ──────────────────────────────────────────────────────

void handleStatusCmd(JsonDocument& doc) {
  JsonDocument resp;
  resp["ack"] = "status"; resp["ok"] = true;
  JsonObject d = resp["data"].to<JsonObject>();
  d["name"] = deviceName;
  d["sec"] = false;
  JsonObject sys = d["sys"].to<JsonObject>();
  sys["up"] = (millis() - bootMs) / 1000;
  sys["heap"] = ESP.getFreeHeap();
  JsonObject stats = d["stats"].to<JsonObject>();
  stats["appr"] = apprCount;
  stats["deny"] = denyCount;
  JsonObject bat = d["bat"].to<JsonObject>();
  bat["pct"] = readBatteryPercent();
  bat["mV"] = (int)(readBatteryVoltage() * 1000.0f);
  String s; serializeJson(resp, s);
  sendBuddy(s);
}

void handleHeartbeat(JsonDocument& doc) {
  lastHeartbeatMs = millis();
  hbRunning = doc["running"] | 0;
  hbWaiting = doc["waiting"] | 0;
  if (doc["msg"].is<const char*>()) {
    Serial.printf("[BUDDY HB] %s (run=%d wait=%d)\n",
                  (const char*)doc["msg"], hbRunning, hbWaiting);
  }
  if (doc["prompt"].is<JsonObject>()) {
    JsonObject p = doc["prompt"];
    String newId = p["id"] | "";
    if (newId.length() > 0 && newId != pendingPromptId) {
      pendingPromptId = newId;
      Serial.printf("[BUDDY] prompt %s tool=%s\n",
                    pendingPromptId.c_str(),
                    (const char*)(p["tool"] | ""));
      currentLEDStatus = LED_FLASHING;
      playJingle("thinking");
      if (digitalRead(PIN_AUTO_SWITCH) == LOW) respondPermission("once");
    }
  } else {
    pendingPromptId = "";
    if      (hbWaiting > 0) currentLEDStatus = LED_BREATHING;
    else if (hbRunning > 0) currentLEDStatus = LED_PULSING;
    else                    currentLEDStatus = LED_SOLID;
  }
}

void handleLedCmd(JsonDocument& doc) {
  String mode = String((const char*)(doc["mode"] | "breathing"));
  mode.toUpperCase();
  if      (mode == "BREATHING") currentLEDStatus = LED_BREATHING;
  else if (mode == "PULSING")   currentLEDStatus = LED_PULSING;
  else if (mode == "SOLID")     currentLEDStatus = LED_SOLID;
  else if (mode == "FLASH")     currentLEDStatus = LED_FLASHING;
  else if (mode == "OFF")       currentLEDStatus = LED_OFF;
  sendBuddy("{\"ack\":\"led\",\"ok\":true}");
}

void handleToneCmd(JsonDocument& doc) {
  int f = doc["freq"] | 1000;
  int ms = doc["ms"] | 100;
  if (f > 0 && ms > 0) playTone(f, ms);
  sendBuddy("{\"ack\":\"tone\",\"ok\":true}");
}

void handleJingleCmd(JsonDocument& doc) {
  String n = String((const char*)(doc["name"] | "startup"));
  n.toLowerCase();
  playJingle(n);
  sendBuddy("{\"ack\":\"jingle\",\"ok\":true}");
}

void processLine(const String& raw) {
  if (raw.length() == 0) return;
  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, raw);
  if (err) { Serial.printf("[JSON err] %s\n", err.c_str()); return; }

  if (doc["cmd"].is<const char*>()) {
    String cmd = String((const char*)doc["cmd"]);
    if      (cmd == "status")   handleStatusCmd(doc);
    else if (cmd == "name")     {
      if (doc["name"].is<const char*>()) deviceName = String((const char*)doc["name"]);
      sendBuddy("{\"ack\":\"name\",\"ok\":true}");
    }
    else if (cmd == "owner")    {
      if (doc["name"].is<const char*>()) ownerName = String((const char*)doc["name"]);
      sendBuddy("{\"ack\":\"owner\",\"ok\":true}");
    }
    else if (cmd == "unpair")   { NimBLEDevice::deleteAllBonds(); sendBuddy("{\"ack\":\"unpair\",\"ok\":true}"); }
    else if (cmd == "led")      handleLedCmd(doc);
    else if (cmd == "tone")     handleToneCmd(doc);
    else if (cmd == "jingle")   handleJingleCmd(doc);
    else                        Serial.printf("[CMD?] %s\n", cmd.c_str());
  } else {
    // No "cmd" key — treat as Hardware Buddy heartbeat snapshot
    handleHeartbeat(doc);
  }
}

// ── USB serial ────────────────────────────────────────────────────────────────

void serviceUsbSerial() {
  while (Serial.available()) {
    char c = Serial.read();
    if (c == '\n' || c == '\r') {
      if (usbBuffer.length() > 0) { processLine(usbBuffer); usbBuffer = ""; }
    } else {
      usbBuffer += c;
    }
  }
}

// ── Setup ─────────────────────────────────────────────────────────────────────

void setup() {
  Serial.begin(115200);
  delay(400);
  Serial.println("\n[ClaudeApprover] pure-NUS firmware boot");
  bootMs = millis();

  pinMode(PIN_FUNC_BTN, INPUT_PULLUP);
  pinMode(PIN_RETURN_BTN, INPUT_PULLUP);
  pinMode(PIN_AUTO_SWITCH, INPUT_PULLUP);
  pinMode(PIN_BUZZER, OUTPUT);
  pinMode(PIN_LED, OUTPUT);
  digitalWrite(PIN_LED, HIGH);

  // Initialize NimBLE peripheral with NUS only.
  NimBLEDevice::init("ClaudeApprover");
  NimBLEDevice::setPower(ESP_PWR_LVL_P9);  // +9dBm for solid range

  NimBLEServer* server = NimBLEDevice::createServer();
  server->setCallbacks(&serverCallbacks, false);

  NimBLEService* nus = server->createService(NUS_SERVICE_UUID);
  nusRxChar = nus->createCharacteristic(NUS_RX_UUID,
      NIMBLE_PROPERTY::WRITE | NIMBLE_PROPERTY::WRITE_NR);
  nusRxChar->setCallbacks(&nusCallbacks);
  nusTxChar = nus->createCharacteristic(NUS_TX_UUID,
      NIMBLE_PROPERTY::NOTIFY | NIMBLE_PROPERTY::READ);
  nus->start();

  NimBLEAdvertising* adv = NimBLEDevice::getAdvertising();
  adv->setName("ClaudeApprover");
  adv->addServiceUUID(NUS_SERVICE_UUID);
  adv->enableScanResponse(true);
  adv->start();
  Serial.println("[BLE] advertising NUS as 'ClaudeApprover'");

  playJingle("startup");
}

// ── Loop ──────────────────────────────────────────────────────────────────────

void updateLED();

void loop() {
  serviceUsbSerial();

  bool funcState   = digitalRead(PIN_FUNC_BTN);
  bool returnState = digitalRead(PIN_RETURN_BTN);
  bool switchState = digitalRead(PIN_AUTO_SWITCH);

  // Auto-accept switch transition
  if (switchState != lastSwitchState) {
    if (switchState == LOW) {
      Serial.println("AUTO-ACCEPT: ON");
      currentLEDStatus = LED_SOLID;
      emitSwitchEvent("auto_accept", "on");
      if (pendingPromptId.length() > 0) respondPermission("once");
    } else {
      Serial.println("AUTO-ACCEPT: OFF");
      currentLEDStatus = LED_BREATHING;
      emitSwitchEvent("auto_accept", "off");
    }
    lastSwitchState = switchState;
  }

  // Function button (press/release semantics — middleman holds modifier combo).
  // No buzzer on button presses; LED + Claude Code state hooks do the talking.
  if (funcState == LOW && lastFuncState == HIGH &&
      (millis() - lastFuncPress > debounceDelay)) {
    lastFuncPress = millis();
    Serial.println("FUNCTION pressed");
    if (pendingPromptId.length() > 0) {
      respondPermission("once");
    } else {
      emitButtonEvent("function", "press");
    }
  } else if (funcState == HIGH && lastFuncState == LOW) {
    Serial.println("FUNCTION released");
    emitButtonEvent("function", "release");
  }
  lastFuncState = funcState;

  // Return button (tap semantics)
  if (returnState == LOW && lastReturnState == HIGH &&
      (millis() - lastReturnPress > debounceDelay)) {
    lastReturnPress = millis();
    Serial.println("RETURN pressed");
    digitalWrite(PIN_LED, LOW);
    delay(20);
    digitalWrite(PIN_LED, HIGH);
    emitButtonEvent("return", "tap");
  }
  lastReturnState = returnState;

  // Periodic battery emit
  if (millis() - lastBatteryEmit > batteryEmitInterval) {
    lastBatteryEmit = millis();
    emitBatteryEvent();
  }

  // Heartbeat watchdog
  if (lastHeartbeatMs > 0 && millis() - lastHeartbeatMs > 30000 &&
      currentLEDStatus != LED_BREATHING && currentLEDStatus != LED_OFF) {
    Serial.println("[BUDDY] heartbeat stale");
    currentLEDStatus = LED_BREATHING;
  }

  updateLED();
  delay(10);
}

// ── LED ───────────────────────────────────────────────────────────────────────

void updateLED() {
  unsigned long now = millis();
  switch (currentLEDStatus) {
    case LED_OFF:    digitalWrite(PIN_LED, HIGH); break;
    case LED_SOLID:  digitalWrite(PIN_LED, LOW);  break;
    case LED_BREATHING:
      if (now - lastLEDUpdate > 15) {
        if (ledDirection) { ledBrightness += 5;  if (ledBrightness >= 255) { ledBrightness = 255; ledDirection = false; } }
        else              { ledBrightness -= 5;  if (ledBrightness <= 30)  { ledBrightness = 30;  ledDirection = true;  } }
        analogWrite(PIN_LED, 255 - ledBrightness);
        lastLEDUpdate = now;
      } break;
    case LED_PULSING:
      if (now - lastLEDUpdate > 8) {
        if (ledDirection) { ledBrightness += 10; if (ledBrightness >= 255) { ledBrightness = 255; ledDirection = false; } }
        else              { ledBrightness -= 10; if (ledBrightness <= 50)  { ledBrightness = 50;  ledDirection = true;  } }
        analogWrite(PIN_LED, 255 - ledBrightness);
        lastLEDUpdate = now;
      } break;
    case LED_FLASHING:
      if (now - lastLEDUpdate > 100) {
        ledState = !ledState;
        digitalWrite(PIN_LED, ledState ? LOW : HIGH);
        lastLEDUpdate = now;
      } break;
  }
}

// ── Buzzer ────────────────────────────────────────────────────────────────────

void playTone(int frequency, int duration) {
  tone(PIN_BUZZER, frequency, duration);
}

void playJingle(String name) {
  if      (name == "startup")  { playTone(1000, 100); delay(120); playTone(1200, 100); delay(120); playTone(1500, 150); }
  else if (name == "approved") { playTone(800, 80);   delay(90);  playTone(1200, 120); }
  else if (name == "denied")   { playTone(1000, 80);  delay(90);  playTone(600, 120); }
  else if (name == "thinking") { playTone(1500, 50); }
  else if (name == "waiting")  { playTone(1000, 100); }
}
