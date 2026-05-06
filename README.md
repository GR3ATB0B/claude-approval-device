# Claude Code Approval Device

A small wireless BLE keyboard for approving Claude Code prompts and triggering
voice dictation. Built on a Seeed Studio XIAO ESP32-S3.

The device pairs with your Mac as a normal Bluetooth keyboard. Three physical
controls do specific things:

| Control                | Sends                       | Use it for                      |
|------------------------|-----------------------------|---------------------------------|
| **Function** button    | `Ctrl+Option+F19`           | Wispr Flow voice dictation hotkey |
| **Return** button      | `Enter`                     | Approving Claude Code prompts   |
| **Auto-Accept** switch | spams `Enter` every 1s      | Hands-free YOLO approval mode   |

When you also have it plugged in via USB-C, an MCP server (in `mcp-server/`)
exposes tools so Claude can drive the LED and buzzer for status feedback —
e.g. play an "approved" jingle, set the LED to solid red, etc.

## Hardware

- Seeed Studio XIAO ESP32-S3
- Two momentary push buttons (Function, Return)
- One latching SPST switch (Auto-Accept)
- One passive piezo buzzer
- One blue LED

### Wiring (silkscreen labels on the XIAO)

| Component          | Pin  | Notes                                   |
|--------------------|------|-----------------------------------------|
| Function button    | D0   | other side to GND, internal pull-up     |
| Return button      | D1   | other side to GND, internal pull-up     |
| Auto-Accept switch | D2   | other side to GND, internal pull-up     |
| Buzzer S pin       | D5   | middle pin → 3.3V, − pin → GND          |
| LED cathode        | D8   | LED anode → 3.3V (inverted PWM)         |

## Firmware (`claude_approval_device.ino`)

Requires:

- ESP32 Arduino Core **3.3.7+**
- NimBLE-Arduino **2.3.8+**
- HijelHID_BLEKeyboard **0.5.0+**

```bash
arduino-cli core install esp32:esp32@3.3.8
arduino-cli lib install "NimBLE-Arduino"
arduino-cli lib install "HijelHID_BLEKeyboard"
arduino-cli compile --fqbn esp32:esp32:XIAO_ESP32S3 .
arduino-cli upload  -p /dev/cu.usbmodem101 --fqbn esp32:esp32:XIAO_ESP32S3 .
```

### Pair

1. macOS → System Settings → Bluetooth
2. Power up the device. Look for **ClaudeApprover** in *Nearby Devices*.
3. Click *Connect* / *Pair*.
4. In Wispr Flow → trigger settings, focus the shortcut field, press the
   Function button on the device once. Wispr captures `Ctrl+Option+F19`.
5. Press Return button → sends `Enter` to whichever app is focused.
6. Flick Auto-Accept ON → device sends `Enter` every 1 second until OFF.

## Control channel (`mcp-server/`)

Plug the device into your Mac via USB-C while the MCP server is running.
Claude (Code or Desktop) gets four tools:

| Tool             | Effect                                              |
|------------------|-----------------------------------------------------|
| `set_led_status` | LED mode: BREATHING / PULSING / SOLID / FLASH / OFF |
| `play_tone`      | one-shot buzzer tone (Hz, ms)                       |
| `play_jingle`    | startup / approved / denied / thinking / waiting    |
| `device_status`  | reports the serial connection state                 |

See `mcp-server/README.md` for installation and Claude config.

The keyboard side works **wirelessly via BLE**. The LED/buzzer control channel
runs over **USB serial** because macOS (by Apple's design) does not let
third-party apps write GATT data to a system-bonded BLE HID device. Plug the
device in when sitting at your desk; unplug for wireless use as a keyboard.

## Notes on the BLE library choice

The first iteration used the original T-vK `ESP32-BLE-Keyboard` library on
arduino-esp32 2.x. It pairs with macOS Sonoma+ but the Bluedroid stack throws
`BTM_GetSecurityFlags false` errors and macOS silently drops every HID
notification. Switching to **HijelHID_BLEKeyboard** (NimBLE 2.x stack) on
arduino-esp32 3.x fixes this completely. Worth knowing if you build something
similar.
