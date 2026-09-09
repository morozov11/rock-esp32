---
name: esp-idf-tools
description: Control ESP-IDF projects using the ESP-IDF Tools local MCP server (idf.py mcp-server via EIM). Use to build projects, flash firmware, set targets, and inspect project status and connected serial devices.
---

# ESP-IDF Tools MCP Server & Workflow

This skill guides interaction with the official Espressif `esp-idf-tools` MCP server built into `idf.py` (ESP-IDF v6.0+), managed via Espressif Installation Manager (`eim`).

Reference: [Espressif Developer Blog: ESP-IDF Tools Local MCP Server](https://developer.espressif.com/blog/2026/04/esp-idf-tools-mcp-server/)

## Architecture & Configuration

- **Transport**: Local `stdio`.
- **Command**: `C:\Program Files\eim\eim.exe run "idf.py mcp-server"`
- **Global Config**: `~/.gemini/config/mcp_config.json`
- **Environment**:
  - `IDF_MCP_WORKSPACE_FOLDER`: Path to project root (e.g. `c:\repos\rock-esp32`).
  - `PYTHONUTF8`: `1`.

## Available Tools

- `set_target(target)`: Set target chip (e.g., `esp32p4`, `esp32c6`, `esp32s3`).
- `build_project()`: Run full project build (`idf.py build`).
- `flash_project(port)`: Flash firmware to device on specified COM port (`idf.py -p <port> flash`).
- `clean_project()`: Clean build artifacts (`idf.py clean`).

## Available Resources

- `project://config`: Project configuration and build directory details.
- `project://status`: Current target, IDF version, build status, and artifact presence.
- `project://devices`: List of connected serial ports and devices.

## CLI Fallback (PowerShell on Windows)

When running commands manually or in background tasks on Windows:
```powershell
powershell -ExecutionPolicy Bypass -Command "$env:PYTHONUTF8='1'; . 'C:\Espressif\tools\Microsoft.v6.1.PowerShell_profile.ps1'; idf.py <subcommand>"
```
Or via EIM:
```cmd
eim run "idf.py <subcommand>"
```
