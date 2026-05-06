# Claude Code Approval Device

A small wireless dongle for approving Claude Code prompts and triggering voice
dictation. Built on a Seeed Studio XIAO ESP32-S3.

The device runs **three** transports out of one firmware:

| Surface                | What it does                                                                                  |
|------------------------|-----------------------------------------------------------------------------------------------|
| **BLE HID keyboard**   | Pairs as `ClaudeApprover`. Return button = Enter, Auto-Accept switch = Enter spam, Function button = `Ctrl+Option+F19` (Wispr Flow). Works for terminal Claude Code, any text field. |
| **Hardware Buddy NUS** | Same BLE peripheral also implements Anthropic's [Claude Desktop Buddy](https://github.com/anthropics/claude-desktop-buddy) wire protocol. When connected via Claude Desktop's Developer → Open Hardware Buddy panel, heartbeats drive the LED and Function button sends a structured `permission:once` decision keyed to the active prompt. No keyboard-focus race. |
| **USB serial MCP**     | Plug in the cable, run the bundled MCP server, and Claude can drive LED status + buzzer jingles directly. |

## Inputs / outputs

| Pin (silkscreen) | Component             | Notes                                                |
|------------------|-----------------------|------------------------------------------------------|
| D0               | Function button       | Buddy approve when prompt active, else `Ctrl+Opt+F19` |
| D1               | Return button         | sends Enter via HID                                  |
| D2               | Auto-accept switch    | spams Enter via HID + auto-approves Buddy prompts    |
| D5               | Passive piezo buzzer  | middle pin → 3.3V, − → GND, S → D5                   |
| D8               | Blue LED (PWM)        | anode → 3.3V, cathode → D8 (inverted, LOW = on)      |

## Behaviour summary

When **Hardware Buddy is connected**, the LED reflects what Claude Desktop is doing:

- prompt waiting → **FLASHING** + thinking chirp
- session running → **PULSING**
- session blocked but no permission prompt → **BREATHING** (slow)
- otherwise → **SOLID** (idle, connected)

When Buddy is **not** connected (or no heartbeat for 30s), LED falls back to BREATHING idle and the USB-serial MCP commands take over.

The Function button does the right thing for the moment:

- if a Buddy prompt is pending → sends `{"cmd":"permission","decision":"once"}` → buzzer plays "approved" jingle
- otherwise → sends `Ctrl+Option+F19` → triggers Wispr Flow voice dictation

The Auto-Accept switch:

- when ON: any incoming Buddy prompt is auto-approved immediately, **and** the device also taps Enter every 1s as a CLI fallback (terminal Claude Code that isn't Buddy-aware).

## Build

Requires:

- ESP32 Arduino Core **3.3.7+**
- NimBLE-Arduino **2.3.8+**
- HijelHID_BLEKeyboard **0.5.0+**
- ArduinoJson **7.x**

```bash
arduino-cli core install esp32:esp32@3.3.8
arduino-cli lib install "NimBLE-Arduino" "HijelHID_BLEKeyboard" "ArduinoJson"
arduino-cli compile --fqbn esp32:esp32:XIAO_ESP32S3 claude_approval_device
arduino-cli upload  -p /dev/cu.usbmodem101 --fqbn esp32:esp32:XIAO_ESP32S3 claude_approval_device
```

## Pair

1. **HID side (terminal CLI)** — macOS → System Settings → Bluetooth → connect *ClaudeApprover*. Wispr Flow → trigger settings → press Function button to capture `Ctrl+Option+F19`.
2. **Hardware Buddy side (Claude Desktop)** — Help → Troubleshooting → *Enable Developer Mode* → Developer menu → *Open Hardware Buddy…* → Connect → pick *ClaudeApprover*. The LED switches from breathing to solid once heartbeats arrive.

Both can be paired at the same time.

## MCP server

See [`mcp-server/`](./mcp-server/). Plug in the device via USB-C, install with `uv sync`, wire up to Claude Code or Claude Desktop via `~/.claude.json` or the Desktop config. Tools:

- `set_led_status` — BREATHING / PULSING / SOLID / FLASH / OFF
- `play_tone` — frequency + duration
- `play_jingle` — startup / approved / denied / thinking / waiting
- `device_status` — serial connection state

The MCP server only matters when the device is plugged in. Untethered, the BLE HID and Hardware Buddy paths still work.

## Why this layering

Apple deliberately hides system-bonded BLE HID peripherals from third-party CoreBluetooth scans. That blocks the obvious "open a custom GATT characteristic on the same peripheral and write commands to it" approach. The Hardware Buddy protocol works because Claude Desktop pairs the NUS service through its own in-app CoreBluetooth flow rather than the OS HID system. We advertise both services on the same peripheral; the OS bonds the HID half, Claude Desktop bonds the NUS half independently. Best of both worlds.

The earlier T-vK `ESP32-BLE-Keyboard` library on arduino-esp32 2.x produced `BTM_GetSecurityFlags false` errors on macOS Sonoma+ and macOS silently dropped every HID notification. Switching to HijelHID_BLEKeyboard (NimBLE 2.x stack) on arduino-esp32 3.x fixed it.
