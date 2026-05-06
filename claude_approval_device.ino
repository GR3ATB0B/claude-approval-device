/*
 * Claude Code Approval Device - BLE HID (HijelHID library)
 * Hardware: XIAO ESP32-S3
 *
 * BLE keyboard via HijelHID_BLEKeyboard (NimBLE 2.x stack).
 * Pair "ClaudeApprover" in Mac Bluetooth settings.
 *
 * WIRING (XIAO ESP32-S3 silkscreen):
 *   D0 - Function button -> GND
 *   D1 - Return button   -> GND
 *   D2 - Auto-accept switch -> GND
 *   D5 - Buzzer S pin
 *   D8 - Blue LED (inverted: LOW = on)
 *
 * Requirements:
 *   ESP32 Arduino Core 3.3.8
 *   NimBLE-Arduino 2.5.0
 *   HijelHID_BLEKeyboard 0.5.0
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
  Serial.println("\n[ClaudeApprover-BLE] boot");

  pinMode(PIN_FUNC_BTN, INPUT_PULLUP);
  pinMode(PIN_RETURN_BTN, INPUT_PULLUP);
  pinMode(PIN_AUTO_SWITCH, INPUT_PULLUP);
  pinMode(PIN_BUZZER, OUTPUT);
  pinMode(PIN_LED, OUTPUT);
  digitalWrite(PIN_LED, HIGH);

  keyboard.setLogLevel(HIDLogLevel::Normal);
  keyboard.begin();
  Serial.println("[ClaudeApprover-BLE] BLE advertising as 'ClaudeApprover'");

  playJingle("startup");
  currentLEDStatus = LED_BREATHING;
}

void loop() {
  handleSerialCommands();

  bool funcState = digitalRead(PIN_FUNC_BTN);
  bool returnState = digitalRead(PIN_RETURN_BTN);
  bool switchState = digitalRead(PIN_AUTO_SWITCH);

  static unsigned long lastDbg = 0;
  if (millis() - lastDbg > 2000) {
    lastDbg = millis();
    Serial.printf("[DBG] FUNC=%d RET=%d SW=%d paired=%d\n",
                  funcState, returnState, switchState, keyboard.isPaired());
  }

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
      Serial.println("Auto-spam: RETURN");
      lastAutoSpam = millis();
      digitalWrite(PIN_LED, LOW);
      delay(10);
      digitalWrite(PIN_LED, HIGH);
    }
  }

  // Function button -> Ctrl+Option+F19 (Wispr Flow trigger, 3-key combo)
  if (funcState == LOW && lastFuncState == HIGH &&
      (millis() - lastFuncPress > debounceDelay)) {
    lastFuncPress = millis();
    Serial.printf("FUNCTION pressed (paired=%d)\n", keyboard.isPaired());
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
    Serial.printf("RETURN pressed (paired=%d)\n", keyboard.isPaired());
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
  Serial.print("Command: ");
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
