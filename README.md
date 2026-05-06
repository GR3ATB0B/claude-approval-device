# Claude Code Approval Device

BLE HID keyboard for approving Claude Code prompts and triggering Wispr Flow voice dictation.

Hardware: Seeed Studio XIAO ESP32-S3.

## Buttons

| Input            | Pin (silkscreen) | Action                              |
|------------------|------------------|-------------------------------------|
| Function button  | D0               | Ctrl+Option+F19 (Wispr Flow hotkey) |
| Return button    | D1               | Enter                               |
| Auto-accept switch | D2             | Spam Enter every 1s while held ON   |

## Wiring

- Each button/switch: one pin to the GPIO listed above, other pin to **GND**. INPUT_PULLUP, active-low.
- Buzzer (passive): S pin → D5, middle pin → 3.3V, − → GND.
- Blue status LED: anode → 3.3V, cathode → D8 (inverted: LOW = on, PWM brightness via `analogWrite`).

## Build

Requires:
- Arduino CLI or Arduino IDE 2.x
- ESP32 Arduino Core 3.3.7+
- NimBLE-Arduino 2.3.8+
- HijelHID_BLEKeyboard 0.5.0+

```
arduino-cli core install esp32:esp32@3.3.8
arduino-cli lib install "NimBLE-Arduino@2.5.0"
arduino-cli lib install "HijelHID_BLEKeyboard"
arduino-cli compile --fqbn esp32:esp32:XIAO_ESP32S3 .
arduino-cli upload -p /dev/cu.usbmodem101 --fqbn esp32:esp32:XIAO_ESP32S3 .
```

## Pair

1. Mac/iOS Bluetooth settings → look for **ClaudeApprover** in nearby devices
2. Connect / Pair
3. In Wispr Flow trigger settings, focus the shortcut field, press Function button — Wispr captures Ctrl+Option+F19
4. Press Return button to send Enter, flick switch to auto-spam Enter

## LED status

Controllable from Serial port over USB:

| Command            | LED state    |
|--------------------|--------------|
| `STATUS:BREATHING` | breathing    |
| `STATUS:PULSING`   | pulsing fast |
| `STATUS:SOLID`     | solid on     |
| `STATUS:FLASH`     | strobe       |
| `STATUS:OFF`       | off          |

Buzzer:

| Command                  | Sound                             |
|--------------------------|-----------------------------------|
| `TONE:freq,duration_ms`  | one tone (e.g. `TONE:1000,200`)   |
| `JINGLE:startup`         | startup chime                     |
| `JINGLE:approved`        | ascending two-tone                |
| `JINGLE:denied`          | descending two-tone               |
| `JINGLE:thinking`        | quick chirp                       |
| `JINGLE:waiting`         | gentle beep                       |

## Notes

The earlier T-vK `ESP32-BLE-Keyboard` library + Bluedroid stack on arduino-esp32 2.x produced `BTM_GetSecurityFlags false` errors on macOS Sonoma+ (HID notifications were rejected after pairing). Switching to HijelHID_BLEKeyboard on NimBLE 2.x (arduino-esp32 3.x) fixed it.
