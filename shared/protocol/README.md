# Auto Typer LAN Protocol

This directory defines the JSON contract between the Electron controller and the ESP32 firmware.

The TypeScript file is the source of truth for the desktop application. The ESP32 implementation mirrors the same field names in its HTTP handlers.

## Transport

- HTTP JSON for commands and queries.
- Server-sent events at `/api/events` for status, logs, faults, job progress, and keymap updates.

## Authority

- The ESP32 is the authority for the active keymap.
- The desktop application may cache and export keymaps, but writes complete keymap documents back to the ESP32.
