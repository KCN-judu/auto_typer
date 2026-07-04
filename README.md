# Auto Typer

ESP32-S3 automatic typing machine workspace. The repository contains the firmware, Electron desktop controller, shared TypeScript protocol types, hardware test sketches, and reference materials.

## Repository Layout

- `src/auto_typer/`: main ESP32-S3 firmware. It controls the OLED, PCA9685 servo driver, Emm_V5.0 CAN motor drivers, static Wi-Fi connection, and remote motion-group execution over TCP.
- `dist/arduino/AutoTyperCore/`: generated precompiled Arduino library package for `ESP32S3 Dev Module` with `16MB` flash and `OPI` PSRAM.
- `dist/arduino/AutoTyperWorkspace/`: generated offline Arduino workspace with one visible AutoTyper board, Arduino data/toolchain content, the packaged library, and a starter sketch.
- `apps/desktop/`: Electron + Vite + React desktop controller for connecting to the device, creating print tasks, debugging actuators, and maintaining keymaps.
- `shared/protocol/`: shared TypeScript protocol contracts used by the desktop app. The ESP32 firmware mirrors these JSON field names manually.
- `src/test/test/`: standalone Arduino connectivity test sketch for OLED, PCA9685, servo, and CAN motor checks.
- `src/test/docs/`: Typst hardware connectivity test documentation.
- `materials/`: motor, PCB, enclosure, manual, and vendor reference material.

## Common Commands

Commands in this workspace should be run through `rtk`.

```bash
rtk npm run desktop:dev
rtk npm run desktop:build
rtk npm run desktop:pack
rtk npm run desktop:dist:mac
rtk npm run desktop:dist:win
rtk npm run desktop:typecheck
rtk npm run firmware:test
rtk npm run firmware:compile
rtk npm run firmware:package
rtk npm run firmware:package:verify
rtk npm run firmware:workspace
rtk npm run firmware:workspace:verify
```

## Firmware Entry Points

- `src/auto_typer/auto_typer.ino`: thin Arduino lifecycle entry. `setup()` and `loop()` delegate to the public firmware API in `AutoTyperFirmware.h`.
- `src/auto_typer/AutoTyperConfig.h`: public sketch-facing firmware config for injected Wi-Fi credentials.
- `src/auto_typer/network/StaticWifiConnector.h`: static Wi-Fi connector. It consumes sketch-provided Wi-Fi credentials, retries without blocking the runtime forever, and gates TCP startup until Wi-Fi is connected.
- `src/auto_typer/auto_typer_config.h`: default hardware, Wi-Fi, CAN, servo, motion, and machine topology configuration.
- `src/auto_typer/auto_typer_types.h`: core enums and data structures for job state, device mode, motion profiles, motor feedback, and remote groups.
- `src/auto_typer/typing_logic.h`: pure planning logic that maps text and keymaps to typing plans.
- `src/auto_typer/auto_typer_runtime.h`: application state machine and action execution.
- `src/auto_typer/transport/GroupCommandServer.h`: NDJSON-over-TCP command stream on port `7777`.
- `src/auto_typer/can/`: CAN bus, TX queue, RX task, motor feedback store, EMM_V5 event parsing, and protocol trace support.

## Control Model

The firmware has two distinct external interactions:

- Setup path: sketch code owns Wi-Fi credentials and passes them to `auto_typer::autoTyperSetup(const FirmwareConfig&)`. The source sketch at `src/auto_typer/auto_typer.ino` still uses a local `config/Secrets.h` for source builds, while the generated Arduino package expects a sketch-local `Secrets.h`.
- Control path: after Wi-Fi is ready, the desktop connects to TCP port `7777` and speaks line-delimited JSON. The desktop submits bounded `exec_group` motion batches, subscribes to telemetry, and closes the overall remote print task with `finish_task`.

## Arduino Packaging

- `rtk npm run firmware:compile`: source-verify the editable firmware tree with repo-local Arduino build artifacts.
- `rtk npm run firmware:package`: rebuild `dist/arduino/AutoTyperCore/` and archive the firmware core into `src/esp32s3/libauto_typer_core.a`.
- `rtk npm run firmware:package:verify`: compile the generated package as a consumer sketch would.
- `rtk npm run firmware:package:upload`: compile and upload the generated package example. Set `AUTO_TYPER_WIFI_SSID` and `AUTO_TYPER_WIFI_PASSWORD` first, and pass `-- --port=/dev/cu.usbmodem...` to override auto-detection when needed.
- The generated package bundles the required third-party Arduino library objects into `libauto_typer_core.a`, so consumers do not need to separately install the Adafruit or ArduinoJson libraries.
- `rtk npm run firmware:workspace`: build an offline Arduino workspace with one visible AutoTyper board, the packaged library, and a starter sketch.
- `rtk npm run firmware:workspace:verify`: verify that the offline workspace exposes only the custom board and compiles the starter sketch successfully.

Arduino IDE target settings for the packaged library:

- Board: `ESP32S3 Dev Module`
- Flash Size: `16MB (128Mb)`
- Flash Mode: `OPI 80MHz`
- PSRAM: `OPI PSRAM`
- Partition Scheme: `16M Flash (3MB APP/9.9MB FATFS)`

The runtime semantics are:

- `exec_group` is admission plus execution of one bounded motion group, not a whole print job.
- `group_accepted` only means the group was admitted into firmware runtime state.
- `group_started`, `block_started`, `block_done`, `group_done`, and `group_final` describe execution progress and terminal outcome.
- `telemetry`, `motor_event`, `motor_state_update`, and `telemetry_overflow` are observability channels, not completion guarantees.
- `reset_fault`, `cancel`, `release_line_feed_origin`, `probe`, and `press_diag_m5` are maintenance or diagnostic commands on the same TCP control stream.

The protocol route and message types live in `shared/protocol/auto-typer-protocol.ts`.

## Desktop Packaging

- `rtk npm run desktop:pack`: build the Electron app and generate an unpacked application bundle in `apps/desktop/release/`.
- `rtk npm run desktop:dist:mac`: build macOS distributables (`.dmg` and `.zip`) on a Mac.
- `rtk npm run desktop:dist:win`: build the Windows portable package on a Windows machine.
- The packaging pipeline uses `electron-builder` with assets from `apps/desktop/resources/`.
- Without Apple Developer signing credentials, the macOS app is still distributable, but Gatekeeper will treat it as an unidentified developer app on first launch.
- When signing credentials are available later, `electron-builder` can use standard environment variables such as `CSC_LINK`, `CSC_KEY_PASSWORD`, `APPLE_ID`, `APPLE_APP_SPECIFIC_PASSWORD`, `APPLE_TEAM_ID`, `WIN_CSC_LINK`, and `WIN_CSC_KEY_PASSWORD`.
- The current repository intentionally leaves signing disabled by default so local and CI packaging can succeed before certificate setup.

## Safety Notes

CAN transmit success is not the same as mechanical completion. The current firmware has motor feedback, CAN diagnostics, and protocol trace support, but motion execution still relies heavily on local estimates, cached motor feedback, and timeout windows. Lost steps, jams, missed sync triggers, stale feedback, or insufficient wait estimates can still make software state diverge from the machine.

任务执行中仍有阻塞式动作和等待；不要把 group 被接受、某条事件已返回，直接理解为真实机械动作已经完成。
