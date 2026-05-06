"""MCP server for the Claude Code Approval Device.

This is a thin shim that translates Claude Code MCP tool calls into HTTP
requests to the Claude Approver menu-bar app, which owns the BLE bond to the
device. The menu-bar app must be running for these tools to work.

Tools exposed:
    set_led_status(status)       BREATHING | PULSING | SOLID | FLASH | OFF
    play_tone(frequency_hz, ms)  any tone on the buzzer
    play_jingle(name)            startup | approved | denied | thinking | waiting
    device_status()               BLE connection state via the menu-bar app
"""

from __future__ import annotations

import os

import httpx
from mcp.server.fastmcp import FastMCP

HOST = os.environ.get("APPROVER_HOST", "127.0.0.1")
PORT = int(os.environ.get("APPROVER_PORT", "47823"))
BASE = f"http://{HOST}:{PORT}"
TIMEOUT = float(os.environ.get("APPROVER_TIMEOUT", "3.0"))

mcp = FastMCP("claude-approval-device")

VALID_STATUSES = {"BREATHING", "PULSING", "SOLID", "FLASH", "OFF"}
VALID_JINGLES = {"startup", "approved", "denied", "thinking", "waiting"}


def _post(path: str, body: dict) -> str:
    try:
        r = httpx.post(f"{BASE}{path}", json=body, timeout=TIMEOUT)
        return f"{path}: {r.status_code} {r.text}"
    except httpx.HTTPError as e:
        return f"{path}: error {e} (is the Claude Approver menu-bar app running?)"


def _get(path: str) -> str:
    try:
        r = httpx.get(f"{BASE}{path}", timeout=TIMEOUT)
        return f"{path}: {r.status_code} {r.text}"
    except httpx.HTTPError as e:
        return f"{path}: error {e} (is the Claude Approver menu-bar app running?)"


@mcp.tool()
def set_led_status(status: str) -> str:
    """Set the device's blue LED status mode.

    Args:
        status: One of BREATHING, PULSING, SOLID, FLASH, OFF.
    """
    s = status.upper().strip()
    if s not in VALID_STATUSES:
        return f"invalid status '{status}'. valid: {sorted(VALID_STATUSES)}"
    return _post("/led", {"mode": s.lower()})


@mcp.tool()
def play_tone(frequency_hz: int, duration_ms: int) -> str:
    """Play a single tone on the device buzzer.

    Args:
        frequency_hz: Tone frequency in Hz (e.g. 1000).
        duration_ms: Duration in milliseconds (e.g. 200).
    """
    if frequency_hz <= 0 or duration_ms <= 0:
        return "frequency and duration must be positive"
    return _post("/tone", {"freq": frequency_hz, "ms": duration_ms})


@mcp.tool()
def play_jingle(name: str) -> str:
    """Play a preset jingle on the buzzer.

    Args:
        name: One of startup, approved, denied, thinking, waiting.
    """
    n = name.lower().strip()
    if n not in VALID_JINGLES:
        return f"invalid jingle '{name}'. valid: {sorted(VALID_JINGLES)}"
    return _post("/jingle", {"name": n})


@mcp.tool()
def device_status() -> str:
    """Report the BLE connection state via the menu-bar app."""
    return _get("/status")


if __name__ == "__main__":
    mcp.run()
