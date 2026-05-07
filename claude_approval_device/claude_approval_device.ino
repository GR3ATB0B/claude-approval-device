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
#include "esp_sleep.h"
#include "driver/rtc_io.h"

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
LEDStatus currentLEDStatus = LED_FLASHING;  // boot state = "no host yet"

unsigned long lastLEDUpdate = 0;
int ledBrightness = 0;
bool ledDirection = true;
bool ledState = false;

// ── Button / switch state ────────────────────────────────────────────────────

bool lastFuncState = HIGH;
bool lastReturnState = HIGH;
bool lastSwitchState = HIGH;
bool lastSwitchReading = HIGH;
unsigned long lastFuncPress = 0;
unsigned long lastReturnPress = 0;
unsigned long lastSwitchChangeMs = 0;
const unsigned long debounceDelay = 50;
// Switch debounce — short, so a real flip commits even with a marginal
// contact. Tradeoff: physical wiggle may chatter. Hardware fix (reflow
// joints / replace switch) is the real cure if chatter is bad.
const unsigned long switchDebounceDelay = 50;

// Return long-press → enter deep sleep (firmware-level power off).
unsigned long returnPressStartMs = 0;
bool returnLongPressFired = false;
const unsigned long RETURN_LONG_PRESS_MS = 3000;  // 3 s — accidental 2 s hits were sleeping the device

// Auto-sleep: device drops to deep sleep after this much inactivity.
// Any button or the auto-accept switch (EXT1 wake on D0/D1/D2 LOW) brings it back.
unsigned long lastActivityMs = 0;
const unsigned long IDLE_SLEEP_TIMEOUT_MS = 5UL * 60UL * 1000UL;  // 5 min

// Set after EXT1 wake-from-sleep so the still-pressed wake button
// doesn't immediately get interpreted as a fresh button event.
bool ignoreButtonsUntilRelease = false;

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

// Plain XIAO ESP32-S3 (non-Sense) has no internal divider from BAT+ to D9.
// Without an external 100k+100k divider wired by the user, D9 floats and
// reading is meaningless. Detect that case and report "unsupported".
bool batterySensingAvailable() {
  // Sample the pin a few times. If it hovers near 0 OR pinned to rail
  // with no plausible variation, assume nothing is wired to it.
  uint32_t sum = 0;
  for (int i = 0; i < 8; i++) sum += analogRead(PIN_BATTERY);
  int raw = sum / 8;
  // Battery via 2:1 divider on a 3.0–4.2 V cell maps to ~1860–2600 mV at the
  // ADC pin → raw ~2300–3200. Anything outside [200, 3900] = no real signal.
  return raw > 200 && raw < 3900;
}

float readBatteryVoltage() {
  uint32_t mv_pin = analogReadMilliVolts(PIN_BATTERY);
  static uint32_t lastLog = 0;
  if (millis() - lastLog > 5000) {
    lastLog = millis();
    int raw = analogRead(PIN_BATTERY);
    Serial.printf("[BAT] raw=%d mv_pin=%u mv_bat=%u\n",
                  raw, (unsigned)mv_pin, (unsigned)(mv_pin * 2));
  }
  return (mv_pin * 2.0f) / 1000.0f;
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
    // Connected + nothing pending = SOLID (calm "I'm here, ready").
    currentLEDStatus = LED_SOLID;
    emitHello();
    emitCurrentSwitchState();
  }
  void onDisconnect(NimBLEServer* server, NimBLEConnInfo& info, int reason) override {
    centralConnected = false;
    Serial.printf("[BLE] central disconnected (reason=%d)\n", reason);
    // Disconnected = FLASHING (visible "looking for host").
    currentLEDStatus = LED_FLASHING;
    lastActivityMs = millis();
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
  d["fw"] = "1.1";
  String s; serializeJson(d, s);
  sendBuddy(s);
}

// Send the current auto-accept switch state. Called after BLE connect so the
// middleman's timer never desyncs from physical reality (e.g. device booted
// with switch already ON, or middleman restarted while device stayed on).
void emitCurrentSwitchState() {
  emitSwitchEvent("auto_accept", lastSwitchState == LOW ? "on" : "off");
}

void emitBatteryEvent() {
  JsonDocument d;
  d["evt"] = "battery";
  if (batterySensingAvailable()) {
    d["pct"] = readBatteryPercent();
    d["mV"] = (int)(readBatteryVoltage() * 1000.0f);
  } else {
    d["available"] = false;  // no divider wired; app should hide the widget
  }
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

// Approvals/denials are silent. Beeps fire only on power-on, power-off,
// and "Claude is fully done, waiting for you" — that's the policy.
void respondPermission(const char* decision, bool audible = true) {
  (void)audible;
  if (pendingPromptId.length() == 0) return;
  JsonDocument d;
  d["cmd"] = "permission";
  d["id"] = pendingPromptId;
  d["decision"] = decision;
  String s; serializeJson(d, s);
  sendBuddy(s);
  if (String(decision) == "once") apprCount++;
  else                            denyCount++;
  pendingPromptId = "";
  currentLEDStatus = LED_PULSING;  // host is acting on the approval
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
  lastActivityMs  = lastHeartbeatMs;
  int prevRunning = hbRunning;
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
      if (digitalRead(PIN_AUTO_SWITCH) == LOW) respondPermission("once", false);
    }
  } else {
    pendingPromptId = "";
    // LED policy:
    //   Claude working (running > 0)        -> PULSING (visible activity)
    //   Idle / done / waiting for the user  -> SOLID   (calm, attention-ready)
    //   Pending permission prompt           -> FLASHING (handled above)
    if (hbRunning > 0) currentLEDStatus = LED_PULSING;
    else               currentLEDStatus = LED_SOLID;

    // "Done completely" attention beep: running just dropped from >0 to 0.
    // That's the moment Claude finished its turn and is waiting for you.
    if (prevRunning > 0 && hbRunning == 0) {
      playJingle("done");
    }
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

// ── Deep sleep / power ───────────────────────────────────────────────────────

// Enter ESP32-S3 deep sleep. Wake when any of D0/D1/D2 is pulled LOW
// (button press or switch flip). Internal RTC pull-ups stay enabled
// through sleep so the buttons read HIGH at rest.
void enterDeepSleep() {
  if (centralConnected) {
    sendBuddy("{\"evt\":\"sleep\"}");
    delay(50);
  }
  // "Powering down" two-tone descending beep.
  playTone(900, 80);  delay(100);
  playTone(500, 120); delay(140);

  digitalWrite(PIN_LED, HIGH);  // LED off (active-low)

  // Wait for the user to release Return / any held button before sleeping.
  // Otherwise the held-LOW pin would immediately trigger the EXT1 ANY_LOW
  // wake mask and the device would self-wake within milliseconds.
  // Cap the wait so a stuck or shorted button can't lock us awake forever.
  unsigned long releaseStart = millis();
  while ((digitalRead(PIN_RETURN_BTN) == LOW || digitalRead(PIN_FUNC_BTN) == LOW) &&
         millis() - releaseStart < 5000) {
    delay(10);
  }
  delay(150);  // contact settle

  // Configure EXT1 wake. Only arm pins that are currently HIGH — otherwise
  // a pin that's already LOW (e.g. auto-accept switch in the ON position)
  // immediately re-triggers ANY_LOW the moment we enter sleep.
  // D0=GPIO1, D1=GPIO2, D2=GPIO3 — all RTC IOs on S3.
  uint64_t wake_mask = 0;
  if (digitalRead(PIN_FUNC_BTN)    == HIGH) { wake_mask |= (1ULL << 1); rtc_gpio_pullup_en((gpio_num_t)1); rtc_gpio_pulldown_dis((gpio_num_t)1); }
  if (digitalRead(PIN_RETURN_BTN)  == HIGH) { wake_mask |= (1ULL << 2); rtc_gpio_pullup_en((gpio_num_t)2); rtc_gpio_pulldown_dis((gpio_num_t)2); }
  if (digitalRead(PIN_AUTO_SWITCH) == HIGH) { wake_mask |= (1ULL << 3); rtc_gpio_pullup_en((gpio_num_t)3); rtc_gpio_pulldown_dis((gpio_num_t)3); }
  if (wake_mask == 0) {
    // Every input is currently LOW (shouldn't happen after release wait,
    // but guard anyway). Force-arm Return so we can still wake.
    wake_mask = (1ULL << 2);
    rtc_gpio_pullup_en((gpio_num_t)2);
    rtc_gpio_pulldown_dis((gpio_num_t)2);
  }
  Serial.printf("[SLEEP] wake_mask=0x%llx\n", (unsigned long long)wake_mask);
  esp_sleep_enable_ext1_wakeup_io(wake_mask, ESP_EXT1_WAKEUP_ANY_LOW);

  Serial.flush();
  esp_deep_sleep_start();
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

  // Seed switch state from current pin reading so the first transition is
  // a real change, not a phantom one from the HIGH default.
  lastSwitchState   = digitalRead(PIN_AUTO_SWITCH);
  lastSwitchReading = lastSwitchState;
  lastActivityMs    = millis();

  // If we woke from deep sleep, log why and ignore button input until the
  // wake-press is released — otherwise the loop sees a held Return and
  // immediately starts the long-press-to-sleep timer all over again.
  esp_sleep_wakeup_cause_t wake = esp_sleep_get_wakeup_cause();
  if (wake == ESP_SLEEP_WAKEUP_EXT1) {
    Serial.println("[BOOT] woke from deep sleep (EXT1)");
    ignoreButtonsUntilRelease = true;
  }

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

  unsigned long now = millis();
  bool funcState     = digitalRead(PIN_FUNC_BTN);
  bool returnState   = digitalRead(PIN_RETURN_BTN);
  bool switchReading = digitalRead(PIN_AUTO_SWITCH);

  // After a wake-from-sleep, the wake button is still held. Skip all input
  // processing until every button reads HIGH again, then resync state vars
  // so the eventual release isn't interpreted as a fresh tap.
  if (ignoreButtonsUntilRelease) {
    if (funcState == HIGH && returnState == HIGH) {
      lastFuncState        = HIGH;
      lastReturnState      = HIGH;
      lastSwitchReading    = switchReading;
      lastSwitchState      = switchReading;
      lastSwitchChangeMs   = now;
      returnPressStartMs   = 0;
      returnLongPressFired = false;
      lastActivityMs       = now;
      ignoreButtonsUntilRelease = false;
    } else {
      updateLED();
      delay(10);
      return;
    }
  }

  // Auto-accept switch — debounced. Slide switch contacts bounce on flip
  // (and a loose D2 wire flickers); without debounce we'd thrash the
  // middleman's auto-accept timer.
  if (switchReading != lastSwitchReading) {
    lastSwitchReading = switchReading;
    lastSwitchChangeMs = now;
    Serial.printf("[SW] raw flip -> %s\n", switchReading == LOW ? "LOW" : "HIGH");
  }
  if (switchReading != lastSwitchState &&
      now - lastSwitchChangeMs > switchDebounceDelay) {
    lastSwitchState = switchReading;
    lastActivityMs = now;
    // Switch state DOES NOT touch LED. LED is driven by connection state
    // and heartbeat ("Claude running" → PULSING, otherwise → SOLID).
    if (lastSwitchState == LOW) {
      Serial.println("AUTO-ACCEPT: ON");
      emitSwitchEvent("auto_accept", "on");
      if (pendingPromptId.length() > 0) respondPermission("once", false);
    } else {
      Serial.println("AUTO-ACCEPT: OFF");
      emitSwitchEvent("auto_accept", "off");
    }
  }

  // Function button (press/release semantics — middleman holds modifier combo).
  if (funcState == LOW && lastFuncState == HIGH &&
      (now - lastFuncPress > debounceDelay)) {
    lastFuncPress = now;
    lastActivityMs = now;
    Serial.println("FUNCTION pressed");
    if (pendingPromptId.length() > 0) {
      respondPermission("once");  // manual → audible
    } else {
      emitButtonEvent("function", "press");
    }
  } else if (funcState == HIGH && lastFuncState == LOW) {
    Serial.println("FUNCTION released");
    lastActivityMs = now;
    emitButtonEvent("function", "release");
  }
  lastFuncState = funcState;

  // Return button: short tap → Return key. Long press (≥2 s) → deep sleep.
  if (returnState == LOW && lastReturnState == HIGH &&
      (now - lastReturnPress > debounceDelay)) {
    lastReturnPress = now;
    returnPressStartMs = now;
    returnLongPressFired = false;
    lastActivityMs = now;
    Serial.println("RETURN pressed");
  } else if (returnState == LOW && lastReturnState == LOW &&
             !returnLongPressFired &&
             returnPressStartMs > 0 &&
             now - returnPressStartMs >= RETURN_LONG_PRESS_MS) {
    // Hold threshold reached — power off now, before user lifts the button.
    returnLongPressFired = true;
    Serial.println("RETURN long-press: entering deep sleep");
    enterDeepSleep();
  } else if (returnState == HIGH && lastReturnState == LOW) {
    if (!returnLongPressFired) {
      // Short tap — fire normal Return key event.
      digitalWrite(PIN_LED, LOW);
      delay(20);
      digitalWrite(PIN_LED, HIGH);
      emitButtonEvent("return", "tap");
    }
    returnPressStartMs = 0;
    returnLongPressFired = false;
    lastActivityMs = now;
  }
  lastReturnState = returnState;

  // Periodic battery emit
  if (now - lastBatteryEmit > batteryEmitInterval) {
    lastBatteryEmit = now;
    emitBatteryEvent();
  }

  // Diagnostic: print raw input state every 2 s. Lets you tell from the
  // serial monitor whether the auto-accept switch is even reaching the GPIO
  // (D2/GPIO3) when you flip it.
  static unsigned long lastDiag = 0;
  if (now - lastDiag > 2000) {
    lastDiag = now;
    Serial.printf("[IO] D0(func)=%d D1(ret)=%d D2(sw)=%d  state(sw)=%d  conn=%d\n",
                  digitalRead(PIN_FUNC_BTN),
                  digitalRead(PIN_RETURN_BTN),
                  digitalRead(PIN_AUTO_SWITCH),
                  lastSwitchState,
                  centralConnected ? 1 : 0);
  }

  // Heartbeat watchdog: if Hardware Buddy stops sending heartbeats while
  // BLE is still connected, drop PULSING back to SOLID (we no longer know
  // if Claude is running, so don't keep showing the "working" animation).
  if (centralConnected && lastHeartbeatMs > 0 &&
      now - lastHeartbeatMs > 30000 &&
      currentLEDStatus == LED_PULSING) {
    Serial.println("[BUDDY] heartbeat stale");
    currentLEDStatus = LED_SOLID;
  }

  // Idle auto-sleep. Only fires when the host is gone — if Wispr/Claude is
  // actively connected the device stays awake regardless of physical
  // activity, since the user might walk away mid-session and come back.
  // Press-and-hold Return is the manual override for "off while host alive".
  if (!centralConnected &&
      pendingPromptId.length() == 0 &&
      now - lastActivityMs > IDLE_SLEEP_TIMEOUT_MS) {
    Serial.println("[SLEEP] idle timeout (no host) — entering deep sleep");
    enterDeepSleep();
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
      // Slow, smooth pulse — "thinking" cue, not a strobe.
      if (now - lastLEDUpdate > 18) {
        if (ledDirection) { ledBrightness += 4; if (ledBrightness >= 255) { ledBrightness = 255; ledDirection = false; } }
        else              { ledBrightness -= 4; if (ledBrightness <= 60)  { ledBrightness = 60;  ledDirection = true;  } }
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
  else if (name == "done")     { playTone(1200, 90);  delay(100); playTone(1600, 130); }
  else if (name == "approved") { playTone(800, 80);   delay(90);  playTone(1200, 120); }
  else if (name == "denied")   { playTone(1000, 80);  delay(90);  playTone(600, 120); }
  else if (name == "thinking") { playTone(1500, 50); }
  else if (name == "waiting")  { playTone(1000, 100); }
}
