# claude-approval-device-mcp

Local MCP server. Bridges Claude (Code or Desktop) → USB serial → device firmware.

The device must be plugged in via USB-C while the MCP server is running. (BLE
handles the keyboard side independently — these two channels do not share
state.)

## Install

```bash
cd mcp-server
uv sync
```

## Run standalone (for testing)

```bash
APPROVAL_DEVICE_PORT=/dev/cu.usbmodem101 uv run server.py
```

Default port is `/dev/cu.usbmodem101`. Check yours with `ls /dev/cu.usbmodem*`.

## Wire up to Claude Code

Add to `~/.claude.json` (or per-project `.mcp.json`):

```json
{
  "mcpServers": {
    "approval-device": {
      "command": "uv",
      "args": ["--directory", "/absolute/path/to/repo/mcp-server", "run", "server.py"],
      "env": {
        "APPROVAL_DEVICE_PORT": "/dev/cu.usbmodem101"
      }
    }
  }
}
```

Restart Claude Code. The four tools appear under `mcp__approval-device__*`.

## Wire up to Claude Desktop

`~/Library/Application Support/Claude/claude_desktop_config.json`:

```json
{
  "mcpServers": {
    "approval-device": {
      "command": "uv",
      "args": ["--directory", "/absolute/path/to/repo/mcp-server", "run", "server.py"],
      "env": { "APPROVAL_DEVICE_PORT": "/dev/cu.usbmodem101" }
    }
  }
}
```

Quit + relaunch Claude Desktop.

## Tools

| Tool             | Args                                                | Effect                              |
|------------------|-----------------------------------------------------|-------------------------------------|
| `set_led_status` | `status` (BREATHING/PULSING/SOLID/FLASH/OFF)        | LED animation mode                  |
| `play_tone`      | `frequency_hz`, `duration_ms`                       | One-shot buzzer tone                |
| `play_jingle`    | `name` (startup/approved/denied/thinking/waiting)   | Preset jingle                       |
| `device_status`  | —                                                   | Reports serial connection state     |
