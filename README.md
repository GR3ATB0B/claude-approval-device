# Claude Code Approval Device

A small wireless dongle for approving Claude Code prompts and triggering voice
dictation. Built on a Seeed Studio XIAO ESP32-S3.

```
   buttons / switch   ┌──────────┐    BLE NUS    ┌────────────────┐  CGEvent
  ─────────────────►  │ ESP32-S3 │  ───────────► │  Menu-bar app  │ ─────────► focused window
                      │ firmware │  ◄─────────── │     (Swift)    │            (Terminal, etc.)
   LED / buzzer       └──────────┘  led/jingle   └─────────┬──────┘
                                                            │ HTTP localhost:47823
                                                            ▼
                                                  ┌──────────────────┐
                                                  │  MCP shim (uv)   │ ◄── Claude Code CLI
                                                  └──────────────────┘ ◄── Claude Desktop
```

The device is a generic BLE peripheral, not a system HID keyboard. Apple
deliberately hides system-bonded HID peripherals from third-party apps, so the
firmware advertises only the Nordic UART Service (the same wire format
[Anthropic uses for Claude Desktop's Hardware Buddy panel](https://github.com/anthropics/claude-desktop-buddy)).
The menu-bar app owns the BLE bond and translates physical button presses into
keystrokes via Quartz Event Services.

## Hardware

| Pin (silkscreen) | Component             | Notes                                               |
|------------------|-----------------------|-----------------------------------------------------|
| D0               | Function button       | Wispr Flow trigger / Buddy approve while prompting |
| D1               | Return button         | sends Enter via CGEvent                             |
| D2               | Auto-accept switch    | spams Enter every 1s while ON / auto-approves Buddy prompts |
| D5               | Passive piezo buzzer  | middle pin → 3.3V, − → GND, S → D5                  |
| D8               | Blue LED (PWM)        | anode → 3.3V, cathode → D8 (inverted, LOW = on)     |

## Firmware (`claude_approval_device/`)

```bash
arduino-cli core install esp32:esp32@3.3.8
arduino-cli lib install "NimBLE-Arduino" "ArduinoJson"
arduino-cli compile --fqbn esp32:esp32:XIAO_ESP32S3 claude_approval_device
arduino-cli upload  -p /dev/cu.usbmodem101 --fqbn esp32:esp32:XIAO_ESP32S3 claude_approval_device
```

The device speaks newline-delimited JSON over BLE NUS:

| Direction          | Examples                                                               |
|--------------------|------------------------------------------------------------------------|
| device → host      | `{"evt":"button","id":"return","action":"tap"}`                        |
|                    | `{"evt":"switch","id":"auto_accept","state":"on"}`                     |
|                    | `{"cmd":"permission","id":"req_…","decision":"once"}` (Buddy mode)     |
| host → device      | `{"cmd":"led","mode":"solid"}`                                         |
|                    | `{"cmd":"jingle","name":"approved"}`                                   |
|                    | `{"cmd":"tone","freq":1000,"ms":200}`                                  |
|                    | Hardware Buddy heartbeat snapshots (running/waiting/prompt/...)        |

## Menu-bar app (`menubar-app/`)

Swift Package, builds a `Claude Approver.app` bundle. Owns the BLE bond,
injects keystrokes via `CGEventPost`, and exposes a localhost HTTP API on
`127.0.0.1:47823` for the MCP shim.

```bash
cd menubar-app
./scripts/build-app.sh
cp -R "build/Claude Approver.app" /Applications/
open "/Applications/Claude Approver.app"
```

First launch will prompt for **Bluetooth** and **Accessibility** permissions —
allow both. Bluetooth is needed to maintain the BLE bond; Accessibility is
needed to inject keystrokes into the focused window.

To auto-start on login:

```bash
./scripts/install-launchagent.sh           # install
./scripts/install-launchagent.sh --uninstall  # remove
```

Logs go to `~/Library/Logs/ClaudeApprover.log`.

## MCP shim (`mcp-server/`)

Python MCP server that forwards Claude Code tool calls to the menu-bar app via
HTTP.

```bash
cd mcp-server
uv sync
```

Claude Code config (`~/.claude.json` or per-project `.mcp.json`):

```json
{
  "mcpServers": {
    "approval-device": {
      "command": "uv",
      "args": [
        "--directory", "/absolute/path/to/repo/mcp-server",
        "run", "server.py"
      ]
    }
  }
}
```

Same shape works in `~/Library/Application Support/Claude/claude_desktop_config.json`
for Claude Desktop.

Tools:

| Tool             | Args                                                | Effect                              |
|------------------|-----------------------------------------------------|-------------------------------------|
| `set_led_status` | `status` (BREATHING/PULSING/SOLID/FLASH/OFF)        | LED animation mode                  |
| `play_tone`      | `frequency_hz`, `duration_ms`                       | One-shot buzzer tone                |
| `play_jingle`    | `name` (startup/approved/denied/thinking/waiting)   | Preset jingle                       |
| `device_status`  | —                                                   | BLE connection state from menu-bar app |

## How it ended up here

First iteration tried to make the device a normal BLE keyboard *and* expose a
custom GATT control channel. macOS hides system-bonded HID peripherals from
every third-party app, so the control channel had no way to reach the device.
Anthropic's [`claude-desktop-buddy`](https://github.com/anthropics/claude-desktop-buddy)
solves the same problem by *not* being HID — the device is a generic BLE
peripheral, the host app injects keystrokes via the macOS event system. We
adopted the same architecture.
