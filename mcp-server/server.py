"""MCP server for the Claude Code Approval Device.

Exposes tools so Claude can drive the device's LED and buzzer over the USB
serial control channel. The device must be plugged in via USB-C while the
MCP server is running. (BLE handles the keyboard side independently.)

Tools:
    set_led_status(status)       BREATHING | PULSING | SOLID | FLASH | OFF
    play_tone(frequency_hz, ms)  any tone on the buzzer
    play_jingle(name)            startup | approved | denied | thinking | waiting
    device_status()               serial connection state
"""

from __future__ import annotations

import os
import threading
import time

import serial
from mcp.server.fastmcp import FastMCP

PORT = os.environ.get("APPROVAL_DEVICE_PORT", "/dev/cu.usbmodem101")
BAUD = 115200

mcp = FastMCP("claude-approval-device")

_lock = threading.Lock()
_ser: serial.Serial | None = None


def _connect() -> serial.Serial:
    global _ser
    if _ser and _ser.is_open:
        return _ser
    _ser = serial.Serial(PORT, BAUD, timeout=0.5)
    time.sleep(0.1)
    return _ser


def _send(cmd: str) -> str:
    try:
        port = _connect()
        with _lock:
            port.write((cmd + "\n").encode())
            port.flush()
        return f"sent: {cmd}"
    except serial.SerialException as e:
        return f"serial error: {e} (is the device plugged in at {PORT}?)"


VALID_STATUSES = {"BREATHING", "PULSING", "SOLID", "FLASH", "OFF"}
VALID_JINGLES = {"startup", "approved", "denied", "thinking", "waiting"}


@mcp.tool()
def set_led_status(status: str) -> str:
    """Set the device's blue LED status mode.

    Args:
        status: One of BREATHING, PULSING, SOLID, FLASH, OFF.
    """
    s = status.upper().strip()
    if s not in VALID_STATUSES:
        return f"invalid status '{status}'. valid: {sorted(VALID_STATUSES)}"
    return _send(f"STATUS:{s}")


@mcp.tool()
def play_tone(frequency_hz: int, duration_ms: int) -> str:
    """Play a single tone on the device buzzer.

    Args:
        frequency_hz: Tone frequency in Hz (e.g. 1000).
        duration_ms: Duration in milliseconds (e.g. 200).
    """
    if frequency_hz <= 0 or duration_ms <= 0:
        return "frequency and duration must be positive"
    return _send(f"TONE:{frequency_hz},{duration_ms}")


@mcp.tool()
def play_jingle(name: str) -> str:
    """Play a preset jingle on the buzzer.

    Args:
        name: One of startup, approved, denied, thinking, waiting.
    """
    n = name.lower().strip()
    if n not in VALID_JINGLES:
        return f"invalid jingle '{name}'. valid: {sorted(VALID_JINGLES)}"
    return _send(f"JINGLE:{n}")


@mcp.tool()
def device_status() -> str:
    """Report whether the serial connection to the device is open."""
    try:
        port = _connect()
        return f"connected on {port.port} @ {port.baudrate} baud"
    except serial.SerialException as e:
        return f"disconnected: {e}"


if __name__ == "__main__":
    mcp.run()
