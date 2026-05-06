/*
 * Claude Code Approval Device
 * Hardware: Seeed Studio XIAO ESP32-S3
 *
 * BLE HID keyboard (paired with macOS/iOS/Android/Windows/Linux as
 * "ClaudeApprover"). Three physical inputs:
 *
 *   D0 - Function button -> sends Ctrl+Option+F19  (Wispr Flow trigger)
 *   D1 - Return button   -> sends Enter
 *   D2 - Auto-accept switch (latching) -> spams Enter every 1s while ON
 *
 * Plus visual + audio status (controllable from Mac via USB serial):
 *
 *   D5 - Passive buzzer (jingles + tones)
 *   D8 - Blue LED (PWM, inverted: LOW = on, breathing/pulsing/solid/flash)
 *
 * USB serial control channel (115200 baud). Mac's MCP server pushes commands:
 *
 *   STATUS:BREATHING|PULSING|SOLID|FLASH|OFF
 *   TONE:<freq_hz>,<duration_ms>
 *   JINGLE:startup|approved|denied|thinking|waiting
 *
 * Build:
 *   ESP32 Arduino Core 3.3.7+
 *   NimBLE-Arduino 2.3.8+
 *   HijelHID_BLEKeyboard 0.5.0+
 *   FQBN: esp32:esp32:XIAO_ESP32S3
 */

#include <HijelHID_BLEKeyboard.h>

HijelHID_BLEKeyboard keyboard("ClaudeApprover", "Banana");

#define PIN_FUNC_BTN    D0
#define PIN_RETURN_BTN  D1
#define PIN_AUTO_SWITCH D2
#define PIN_BUZZER      D5
#define PIN_LED         D8

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

String commandBuffer = "";

void setup() {
  Serial.begin(115200);
  delay(500);
  Serial.println("\n[ClaudeApprover] boot");

  pinMode(PIN_FUNC_BTN, INPUT_PULLUP);
  pinMode(PIN_RETURN_BTN, INPUT_PULLUP);
  pinMode(PIN_AUTO_SWITCH, INPUT_PULLUP);
  pinMode(PIN_BUZZER, OUTPUT);
  pinMode(PIN_LED, OUTPUT);
  digitalWrite(PIN_LED, HIGH);

  keyboard.setLogLevel(HIDLogLevel::Normal);
  keyboard.begin();
  Serial.println("[ClaudeApprover] BLE advertising as 'ClaudeApprover'");

  playJingle("startup");
  currentLEDStatus = LED_BREATHING;
}

void loop() {
  handleSerialCommands();

  bool funcState = digitalRead(PIN_FUNC_BTN);
  bool returnState = digitalRead(PIN_RETURN_BTN);
  bool switchState = digitalRead(PIN_AUTO_SWITCH);

  if (switchState != lastSwitchState) {
    if (switchState == LOW) {
      Serial.println("AUTO-ACCEPT: ON");
      currentLEDStatus = LED_SOLID;
      playTone(1000, 100);
    } else {
      Serial.println("AUTO-ACCEPT: OFF");
      currentLEDStatus = LED_BREATHING;
      playTone(800, 50);
      delay(100);
      playTone(800, 50);
    }
    lastSwitchState = switchState;
  }

  if (switchState == LOW && keyboard.isPaired()) {
    if (millis() - lastAutoSpam >= autoSpamInterval) {
      keyboard.tap(KEY_RETURN);
      lastAutoSpam = millis();
      digitalWrite(PIN_LED, LOW);
      delay(10);
      digitalWrite(PIN_LED, HIGH);
    }
  }

  // Function button -> Ctrl+Option+F19 (3-key combo, fits Wispr Flow's limit)
  if (funcState == LOW && lastFuncState == HIGH &&
      (millis() - lastFuncPress > debounceDelay)) {
    lastFuncPress = millis();
    Serial.println("FUNCTION pressed");
    if (keyboard.isPaired()) {
      keyboard.press(KEY_F19, KEY_MOD_LCTRL | KEY_MOD_LALT);
    }
    playTone(1200, 50);
  } else if (funcState == HIGH && lastFuncState == LOW) {
    Serial.println("FUNCTION released");
    if (keyboard.isPaired()) {
      keyboard.releaseAll();
    }
    playTone(1000, 50);
  }
  lastFuncState = funcState;

  if (returnState == LOW && lastReturnState == HIGH &&
      (millis() - lastReturnPress > debounceDelay)) {
    lastReturnPress = millis();
    Serial.println("RETURN pressed");
    if (keyboard.isPaired()) {
      keyboard.tap(KEY_RETURN);
    }
    playTone(1500, 80);
    digitalWrite(PIN_LED, LOW);
    delay(20);
    digitalWrite(PIN_LED, HIGH);
  }
  lastReturnState = returnState;

  updateLED();
  delay(10);
}

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
