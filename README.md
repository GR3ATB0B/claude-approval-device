# Claude Code Approval Device

A small wireless dongle for approving Claude Code prompts and triggering voice
dictation. Built on a Seeed Studio XIAO ESP32-S3.

```
   buttons / switch   ┌──────────┐    BLE NUS    ┌────────────────┐  CGEvent
  ─────────────────►  │ ESP32-S3 │  ───────────► │  Menu-bar app  │ ─────────► focused window
                      │ firmware │  ◄─────────── │     (Swift)    │            (Terminal, etc.)
   LED / buzzer       └──────────┘  led/jingle   └─────────┬──────┘
                                                            │ HTTP localhost:47823
                                          Claude Code hooks ▼
                                                  ┌──────────────────┐
                                                  │ /thinking/start  │ ◄── UserPromptSubmit
                                                  │ /thinking/stop   │ ◄── Stop / StopFailure
                                                  │ /led /jingle     │ ◄── MCP shim (uv)
                                                  └──────────────────┘
```

The device is a generic BLE peripheral, not a system HID keyboard. Apple
deliberately hides system-bonded HID peripherals from third-party apps, so the
firmware advertises only the Nordic UART Service (the same wire format
[Anthropic uses for Claude Desktop's Hardware Buddy panel](https://github.com/anthropics/claude-desktop-buddy)).
The menu-bar app owns the BLE bond and translates physical button presses into
keystrokes via Quartz Event Services.

## Hardware

| Pin (silkscreen) | Component            | Notes                                                                 |
|------------------|----------------------|-----------------------------------------------------------------------|
| D0               | Function button      | Wispr Flow trigger (Ctrl+Opt+F19) / Buddy approve while prompting     |
| D1               | Return button        | tap → sends Enter; press-hold ≥ 2 s → device deep-sleeps              |
| D2               | Auto-accept switch   | bypassed by a software toggle in the menu (hardware is unreliable)    |
| D5               | Passive piezo buzzer | middle pin → 3.3 V, − → GND, S → D5                                   |
| D8               | Blue LED (PWM)       | anode → 3.3 V, cathode → D8 (inverted, LOW = on)                      |
| D9 / GPIO8       | Battery sense (opt.) | needs an external 100 kΩ + 100 kΩ divider from BAT+ — see below       |

### Battery sense

Plain XIAO ESP32-S3 (non-Sense) has no internal divider from `BAT+` to a GPIO,
so without an external divider the firmware reports
`{"evt":"battery","available":false}` and the menu-bar app hides its battery
widget. To enable monitoring, solder two 100 kΩ resistors in series between
`BAT+`, `D9`, and `GND`; the firmware already expects a 2:1 ratio.

## Behavior

### LED

| State                                    | Animation |
|------------------------------------------|-----------|
| Disconnected (no central bonded)         | flashing  |
| Connected, idle                          | solid     |
| Connected, Claude is thinking            | pulsing   |
| Permission prompt pending (Buddy mode)   | flashing  |

### Buzzer

The buzzer is intentionally quiet. It fires on exactly three events:

- **Power on** (startup three-tone rising)
- **Power off** (descending two-tone, when the device is going to deep sleep)
- **Claude finished thinking** (two-tone rising "done" jingle)

No beep on prompts, approvals, or button presses.

### Power

- **Press-hold Return ≥ 2 s** → device plays the power-off tone and enters deep
  sleep (~10 µA). Releasing the button before the threshold cancels the action.
- **5 minutes idle with no host connected** → automatic deep sleep.
- **Wake** → press any unblocked button (D0 / D1, plus D2 if it isn't currently
  LOW). EXT1 wake mask is computed at sleep time so a stuck-LOW pin can't
  self-trigger an instant wake. BLE reconnects in ~2-3 s.

### Auto-accept toggle (software)

The D2 hardware switch is unreliable on the current build (a stuck contact
pins the line LOW), so the menu-bar app exposes a big green
**AUTO-ACCEPT: ON / OFF** button at the top of its dropdown. While ON it
spams the Return key every 1 s into the focused window. Switch events from
the device are ignored while this toggle is the source of truth.

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
|                    | `{"evt":"switch","id":"auto_accept","state":"on"}` *(ignored by host)* |
|                    | `{"evt":"battery","available":false}` *(or `{"pct":N,"mV":N}`)*        |
|                    | `{"evt":"sleep"}` *(emitted just before deep sleep)*                   |
|                    | `{"cmd":"permission","id":"req_…","decision":"once"}` *(Buddy mode)*   |
| host → device      | `{"cmd":"led","mode":"solid"}` (`solid`/`pulsing`/`flash`/`breathing`/`off`) |
|                    | `{"cmd":"jingle","name":"done"}`                                       |
|                    | `{"cmd":"tone","freq":1000,"ms":200}`                                  |
|                    | Hardware Buddy heartbeat snapshots (running/waiting/prompt/...)        |

USB CDC accepts the same JSON commands at 115200 baud, useful for debugging.
The firmware also prints `[IO]` and `[BAT]` diagnostic lines every few seconds
so you can see button / switch / battery state from `arduino-cli monitor`.

## Menu-bar app (`menubar-app/`)

Swift Package, builds a `Claude Approver.app` bundle. Owns the BLE bond,
injects keystrokes via `CGEventPost`, and exposes a localhost HTTP API on
`127.0.0.1:47823` for the MCP shim *and* for Claude Code lifecycle hooks.

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

### HTTP routes (loopback only)

| Method | Path               | Body                                  | Effect                                            |
|--------|--------------------|---------------------------------------|---------------------------------------------------|
| GET    | `/status`          | —                                     | BLE state, device id, last incoming line          |
| POST   | `/led`             | `{"mode":"solid"}` etc.               | LED animation mode                                |
| POST   | `/jingle`          | `{"name":"done"}` etc.                | Preset jingle                                     |
| POST   | `/tone`            | `{"freq":1000,"ms":200}`              | One-shot buzzer tone                              |
| POST   | `/thinking/start`  | —                                     | Sets LED `pulsing` (called by `UserPromptSubmit`) |
| POST   | `/thinking/stop`   | —                                     | Sets LED `solid` + plays `done` (called by `Stop`)|

## Claude Code integration

Add this `hooks` block to `~/.claude/settings.json` (merge with any existing
hooks entry — don't overwrite the file):

```json
"hooks": {
  "UserPromptSubmit": [
    {
      "hooks": [
        {
          "type": "command",
          "command": "curl -s -m 1 -X POST http://127.0.0.1:47823/thinking/start >/dev/null 2>&1 || true",
          "async": true,
          "timeout": 2
        }
      ]
    }
  ],
  "Stop": [
    {
      "hooks": [
        {
          "type": "command",
          "command": "curl -s -m 1 -X POST http://127.0.0.1:47823/thinking/stop >/dev/null 2>&1 || true",
          "async": true,
          "timeout": 2
        }
      ]
    }
  ],
  "StopFailure": [
    {
      "hooks": [
        {
          "type": "command",
          "command": "curl -s -m 1 -X POST http://127.0.0.1:47823/thinking/stop >/dev/null 2>&1 || true",
          "async": true,
          "timeout": 2
        }
      ]
    }
  ]
}
```

`UserPromptSubmit` fires the moment you hit Enter; `Stop` / `StopFailure`
fire when the assistant turn ends. The device pulses while Claude is
thinking and beeps when it goes back to solid.

## MCP shim (`mcp-server/`)

Python MCP server that forwards Claude Code tool calls to the menu-bar app
via the same localhost API.

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

| Tool             | Args                                                | Effect                                  |
|------------------|-----------------------------------------------------|-----------------------------------------|
| `set_led_status` | `status` (BREATHING/PULSING/SOLID/FLASH/OFF)        | LED animation mode                      |
| `play_tone`      | `frequency_hz`, `duration_ms`                       | One-shot buzzer tone                    |
| `play_jingle`    | `name` (startup/done/approved/denied/thinking/...)  | Preset jingle                           |
| `device_status`  | —                                                   | BLE connection state from menu-bar app  |

## How it ended up here

First iteration tried to make the device a normal BLE keyboard *and* expose
a custom GATT control channel. macOS hides system-bonded HID peripherals
from every third-party app, so the control channel had no way to reach the
device. Anthropic's
[`claude-desktop-buddy`](https://github.com/anthropics/claude-desktop-buddy)
solves the same problem by *not* being HID — the device is a generic BLE
peripheral, the host app injects keystrokes via the macOS event system. We
adopted the same architecture.

The next round of pain came from the LED / buzzer policy: every prompt,
heartbeat, and approval was firing a jingle, which sounded like a smoke
alarm during long sessions. The current rules — three discrete beeps
(power on, power off, "Claude is done") and an LED state driven solely by
connection + thinking state — are the result of tightening that down.
