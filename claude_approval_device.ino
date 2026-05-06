/*
 * Claude Code Approval Device — dual-mode firmware
 * Hardware: Seeed Studio XIAO ESP32-S3
 *
 * Two BLE roles served from one peripheral:
 *
 * 1. BLE HID keyboard ("ClaudeApprover") — for Claude Code CLI in a terminal,
 *    or any app that listens for keyboard input. Buttons send key combos.
 * 2. Anthropic Hardware Buddy NUS service — for Claude Desktop / Claude
 *    Cowork via Developer → Open Hardware Buddy. Receives heartbeats with
 *    permission prompts; sends structured permission decisions back.
 *
 * Same firmware speaks both. Mac may system-pair the HID; Claude Desktop
 * connects to the NUS service via CoreBluetooth.
 *
 * Buttons:
 *   D0 - Function     -> Hardware Buddy "once" approval if a prompt is
 *                        pending; otherwise sends Ctrl+Option+F19 (Wispr Flow)
 *   D1 - Return       -> sends Enter via HID
 *   D2 - Auto-accept  -> while ON, auto-approves any incoming prompt and
 *                        also keeps spamming Enter every 1s as a CLI fallback
 *
 * Status:
 *   D5 - passive buzzer (jingles + tones)
 *   D8 - blue LED (PWM, inverted: LOW = on)
 *
 *   LED is driven by Hardware Buddy heartbeats when connected:
 *     prompt waiting   -> FLASH + thinking jingle once
 *     running > 0      -> PULSING
 *     waiting > 0      -> BREATHING (slow)
 *     otherwise        -> SOLID (idle, connected)
 *   When no Buddy connection, USB serial commands win (STATUS:/JINGLE:/TONE:).
 *
 * Requires:
 *   esp32 core 3.3.7+, NimBLE-Arduino 2.3.8+, HijelHID_BLEKeyboard 0.5.0+,
 *   ArduinoJson 7.x
 */

#include <HijelHID_BLEKeyboard.h>
#include <NimBLEDevice.h>
#include <ArduinoJson.h>

HijelHID_BLEKeyboard keyboard("ClaudeApprover", "Banana");

#define PIN_FUNC_BTN    D0
#define PIN_RETURN_BTN  D1
#define PIN_AUTO_SWITCH D2
#define PIN_BUZZER      D5
#define PIN_LED         D8

// ── Hardware Buddy BLE NUS protocol (matches Anthropic spec) ─────────────────
#define NUS_SERVICE_UUID "6e400001-b5a3-f393-e0a9-e50e24dcca9e"
#define NUS_RX_UUID      "6e400002-b5a3-f393-e0a9-e50e24dcca9e"  // host -> device
#define NUS_TX_UUID      "6e400003-b5a3-f393-e0a9-e50e24dcca9e"  // device -> host

NimBLECharacteristic* nusRxChar = nullptr;
NimBLECharacteristic* nusTxChar = nullptr;
String nusRxBuffer;

// Buddy state
String pendingPromptId;
String pendingPromptTool;
String pendingPromptHint;
unsigned long lastHeartbeatMs = 0;
int hbRunning = 0, hbWaiting = 0, hbTotal = 0;
String deviceName = "Clawd";
String ownerName  = "";
uint32_t apprCount = 0, denyCount = 0;
unsigned long bootMs = 0;

void processCommand(String cmd);  // forward decl (USB serial)
void sendBuddy(const String& json);

// LED status
enum LEDStatus { LED_OFF, LED_BREATHING, LED_PULSING, LED_SOLID, LED_FLASHING };
LEDStatus currentLEDStatus = LED_BREATHING;

unsigned long lastLEDUpdate = 0;
int ledBrightness = 0;
bool ledDirection = true;
bool ledState = false;

bool lastFuncState = HIGH;
bool lastReturnState = HIGH;
bool lastSwitchState = HIGH;

unsigned long lastFuncPress = 0;
unsigned long lastReturnPress = 0;
const unsigned long debounceDelay = 50;

unsigned long lastAutoSpam = 0;
const unsigned long autoSpamInterval = 1000;

String commandBuffer = "";  // USB serial line buffer

// ── Buddy command handlers ────────────────────────────────────────────────────

void handleBuddyCmd(JsonDocument& doc) {
  const char* cmd = doc["cmd"];
  if (!cmd) return;
  String c = cmd;

  if (c == "status") {
    JsonDocument resp;
    resp["ack"] = "status";
    resp["ok"] = true;
    JsonObject d = resp["data"].to<JsonObject>();
    d["name"] = deviceName;
    d["sec"] = false;  // we don't bond Buddy link with encryption (yet)
    JsonObject sys = d["sys"].to<JsonObject>();
    sys["up"] = (millis() - bootMs) / 1000;
    sys["heap"] = ESP.getFreeHeap();
    JsonObject stats = d["stats"].to<JsonObject>();
    stats["appr"] = apprCount;
    stats["deny"] = denyCount;
    String out;
    serializeJson(resp, out);
    sendBuddy(out);

  } else if (c == "name") {
    if (doc["name"].is<const char*>()) {
      deviceName = String((const char*)doc["name"]);
    }
    sendBuddy("{\"ack\":\"name\",\"ok\":true}");

  } else if (c == "owner") {
    if (doc["name"].is<const char*>()) {
      ownerName = String((const char*)doc["name"]);
      Serial.printf("[BUDDY] owner is %s\n", ownerName.c_str());
    }
    sendBuddy("{\"ack\":\"owner\",\"ok\":true}");

  } else if (c == "unpair") {
    NimBLEDevice::deleteAllBonds();
    sendBuddy("{\"ack\":\"unpair\",\"ok\":true}");
  }
  // chunk/file/char_begin/char_end ignored — no folder push support yet
}

void handleBuddyHeartbeat(JsonDocument& doc) {
  lastHeartbeatMs = millis();
  hbTotal   = doc["total"]   | 0;
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
      pendingPromptId   = newId;
      pendingPromptTool = String((const char*)(p["tool"] | ""));
      pendingPromptHint = String((const char*)(p["hint"] | ""));
      Serial.printf("[BUDDY] prompt %s tool=%s\n",
                    pendingPromptId.c_str(), pendingPromptTool.c_str());
      currentLEDStatus = LED_FLASHING;
      playJingle("thinking");

      // Auto-accept switch ON: immediately approve.
      if (digitalRead(PIN_AUTO_SWITCH) == LOW) {
        respondPermission("once");
      }
    }
  } else {
    // No active prompt — pick LED mode from session counts.
    pendingPromptId = "";
    if (hbWaiting > 0)      currentLEDStatus = LED_BREATHING;
    else if (hbRunning > 0) currentLEDStatus = LED_PULSING;
    else                    currentLEDStatus = LED_SOLID;
  }
}

void respondPermission(const char* decision) {
  if (pendingPromptId.length() == 0) return;
  JsonDocument out;
  out["cmd"] = "permission";
  out["id"] = pendingPromptId;
  out["decision"] = decision;
  String s;
  serializeJson(out, s);
  sendBuddy(s);
  if (String(decision) == "once") {
    apprCount++;
    playJingle("approved");
  } else {
    denyCount++;
    playJingle("denied");
  }
  pendingPromptId = "";
  currentLEDStatus = LED_PULSING;
}

void processBuddyLine(const String& line) {
  if (line.length() == 0) return;
  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, line);
  if (err) {
    Serial.printf("[BUDDY] JSON parse error: %s\n", err.c_str());
    return;
  }
  if (doc["cmd"].is<const char*>()) {
    handleBuddyCmd(doc);
  } else {
    handleBuddyHeartbeat(doc);
  }
}

class NusRxCallbacks : public NimBLECharacteristicCallbacks {
  void onWrite(NimBLECharacteristic* chr, NimBLEConnInfo& info) override {
    std::string val = chr->getValue();
    nusRxBuffer.concat(val.c_str());
    int nl;
    while ((nl = nusRxBuffer.indexOf('\n')) >= 0) {
      String line = nusRxBuffer.substring(0, nl);
      nusRxBuffer.remove(0, nl + 1);
      processBuddyLine(line);
    }
  }
};

NusRxCallbacks nusCallbacks;

void sendBuddy(const String& json) {
  if (!nusTxChar) return;
  String line = json + "\n";
  // BLE notification MTU is small; NimBLE auto-fragments under setValue+notify.
  nusTxChar->setValue((uint8_t*)line.c_str(), line.length());
  nusTxChar->notify();
  Serial.print("[BUDDY TX] ");
  Serial.println(json);
}

// ── Setup ─────────────────────────────────────────────────────────────────────

void setup() {
  Serial.begin(115200);
  delay(500);
  Serial.println("\n[ClaudeApprover] dual-mode boot");
  bootMs = millis();

  pinMode(PIN_FUNC_BTN, INPUT_PULLUP);
  pinMode(PIN_RETURN_BTN, INPUT_PULLUP);
  pinMode(PIN_AUTO_SWITCH, INPUT_PULLUP);
  pinMode(PIN_BUZZER, OUTPUT);
  pinMode(PIN_LED, OUTPUT);
  digitalWrite(PIN_LED, HIGH);

  keyboard.setLogLevel(HIDLogLevel::Normal);
  keyboard.begin();
  Serial.println("[BLE HID] advertising as 'ClaudeApprover'");

  // Add Hardware Buddy NUS service to the same peripheral.
  NimBLEServer* server = NimBLEDevice::createServer();  // singleton — returns existing
  NimBLEService* nus = server->createService(NUS_SERVICE_UUID);
  nusRxChar = nus->createCharacteristic(
      NUS_RX_UUID,
      NIMBLE_PROPERTY::WRITE | NIMBLE_PROPERTY::WRITE_NR);
  nusRxChar->setCallbacks(&nusCallbacks);
  nusTxChar = nus->createCharacteristic(
      NUS_TX_UUID,
      NIMBLE_PROPERTY::NOTIFY | NIMBLE_PROPERTY::READ);
  nus->start();
  // HijelHID owns the advertising data; we add our service UUID to it.
  NimBLEAdvertising* adv = NimBLEDevice::getAdvertising();
  adv->addServiceUUID(NUS_SERVICE_UUID);
  adv->start();
  Serial.println("[BLE NUS] Hardware Buddy service ready");

  playJingle("startup");
  currentLEDStatus = LED_BREATHING;
}

// ── Loop ──────────────────────────────────────────────────────────────────────

void loop() {
  handleSerialCommands();

  bool funcState = digitalRead(PIN_FUNC_BTN);
  bool returnState = digitalRead(PIN_RETURN_BTN);
  bool switchState = digitalRead(PIN_AUTO_SWITCH);

  if (switchState != lastSwitchState) {
    if (switchState == LOW) {
      Serial.println("AUTO-ACCEPT: ON");
      playTone(1000, 100);
      // If a prompt is already pending, approve it now.
      if (pendingPromptId.length() > 0) respondPermission("once");
    } else {
      Serial.println("AUTO-ACCEPT: OFF");
      playTone(800, 50);
      delay(100);
      playTone(800, 50);
    }
    lastSwitchState = switchState;
  }

  // CLI fallback: while switch ON, also spam Enter via HID for terminal Claude Code.
  if (switchState == LOW && keyboard.isPaired()) {
    if (millis() - lastAutoSpam >= autoSpamInterval) {
      keyboard.tap(KEY_RETURN);
      lastAutoSpam = millis();
      digitalWrite(PIN_LED, LOW);
      delay(10);
      digitalWrite(PIN_LED, HIGH);
    }
  }

  // Function button: prefer Buddy approval when a prompt is live, else Wispr.
  if (funcState == LOW && lastFuncState == HIGH &&
      (millis() - lastFuncPress > debounceDelay)) {
    lastFuncPress = millis();
    if (pendingPromptId.length() > 0) {
      Serial.println("FUNCTION pressed -> Buddy approve(once)");
      respondPermission("once");
    } else {
      Serial.println("FUNCTION pressed -> Ctrl+Option+F19 (Wispr Flow)");
      if (keyboard.isPaired()) {
        keyboard.press(KEY_F19, KEY_MOD_LCTRL | KEY_MOD_LALT);
      }
      playTone(1200, 50);
    }
  } else if (funcState == HIGH && lastFuncState == LOW) {
    if (keyboard.isPaired()) keyboard.releaseAll();
    playTone(1000, 50);
  }
  lastFuncState = funcState;

  // Return button: Enter via HID (works for terminal CLI Claude Code).
  if (returnState == LOW && lastReturnState == HIGH &&
      (millis() - lastReturnPress > debounceDelay)) {
    lastReturnPress = millis();
    Serial.println("RETURN pressed -> Enter");
    if (keyboard.isPaired()) keyboard.tap(KEY_RETURN);
    playTone(1500, 80);
    digitalWrite(PIN_LED, LOW);
    delay(20);
    digitalWrite(PIN_LED, HIGH);
  }
  lastReturnState = returnState;

  // Heartbeat watchdog: if Buddy stops sending for >30s, fall back to breathing idle.
  if (lastHeartbeatMs > 0 && millis() - lastHeartbeatMs > 30000) {
    if (currentLEDStatus != LED_BREATHING && currentLEDStatus != LED_OFF) {
      Serial.println("[BUDDY] heartbeat stale, idle");
      currentLEDStatus = LED_BREATHING;
    }
  }

  updateLED();
  delay(10);
}

// ── USB serial control channel (independent of Buddy) ────────────────────────

void handleSerialCommands() {
  while (Serial.available() > 0) {
    char c = Serial.read();
    if (c == '\n' || c == '\r') {
      if (commandBuffer.length() > 0) {
        processCommand(commandBuffer);
        commandBuffer = "";
      }
    } else {
      commandBuffer += c;
    }
  }
}

void processCommand(String cmd) {
  cmd.trim();
  Serial.print("[CMD] ");
  Serial.println(cmd);

  if (cmd.startsWith("STATUS:")) {
    String status = cmd.substring(7);
    status.toUpperCase();
    if (status == "BREATHING") currentLEDStatus = LED_BREATHING;
    else if (status == "PULSING") currentLEDStatus = LED_PULSING;
    else if (status == "SOLID") currentLEDStatus = LED_SOLID;
    else if (status == "FLASH") currentLEDStatus = LED_FLASHING;
    else if (status == "OFF") currentLEDStatus = LED_OFF;
  } else if (cmd.startsWith("TONE:")) {
    String params = cmd.substring(5);
    int commaIndex = params.indexOf(',');
    if (commaIndex > 0) {
      int frequency = params.substring(0, commaIndex).toInt();
      int duration = params.substring(commaIndex + 1).toInt();
      playTone(frequency, duration);
    }
  } else if (cmd.startsWith("JINGLE:")) {
    String jingle = cmd.substring(7);
    jingle.toLowerCase();
    playJingle(jingle);
  }
}

// ── LED ──────────────────────────────────────────────────────────────────────

void updateLED() {
  unsigned long now = millis();
  switch (currentLEDStatus) {
    case LED_OFF:
      digitalWrite(PIN_LED, HIGH);
      break;
    case LED_SOLID:
      digitalWrite(PIN_LED, LOW);
      break;
    case LED_BREATHING:
      if (now - lastLEDUpdate > 15) {
        if (ledDirection) {
          ledBrightness += 5;
          if (ledBrightness >= 255) { ledBrightness = 255; ledDirection = false; }
        } else {
          ledBrightness -= 5;
          if (ledBrightness <= 30) { ledBrightness = 30; ledDirection = true; }
        }
        analogWrite(PIN_LED, 255 - ledBrightness);
        lastLEDUpdate = now;
      }
      break;
    case LED_PULSING:
      if (now - lastLEDUpdate > 8) {
        if (ledDirection) {
          ledBrightness += 10;
          if (ledBrightness >= 255) { ledBrightness = 255; ledDirection = false; }
        } else {
          ledBrightness -= 10;
          if (ledBrightness <= 50) { ledBrightness = 50; ledDirection = true; }
        }
        analogWrite(PIN_LED, 255 - ledBrightness);
        lastLEDUpdate = now;
      }
      break;
    case LED_FLASHING:
      if (now - lastLEDUpdate > 100) {
        ledState = !ledState;
        digitalWrite(PIN_LED, ledState ? LOW : HIGH);
        lastLEDUpdate = now;
      }
      break;
  }
}

void playTone(int frequency, int duration) {
  tone(PIN_BUZZER, frequency, duration);
}

void playJingle(String name) {
  if (name == "startup") {
    playTone(1000, 100); delay(120);
    playTone(1200, 100); delay(120);
    playTone(1500, 150);
  } else if (name == "approved") {
    playTone(800, 80); delay(90);
    playTone(1200, 120);
  } else if (name == "denied") {
    playTone(1000, 80); delay(90);
    playTone(600, 120);
  } else if (name == "thinking") {
    playTone(1500, 50);
  } else if (name == "waiting") {
    playTone(1000, 100);
  }
}
