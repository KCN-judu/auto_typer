import assert from "node:assert/strict";
import { chmodSync, cpSync, existsSync, mkdirSync, rmSync, writeFileSync } from "node:fs";
import { execFileSync } from "node:child_process";
import { join, resolve } from "node:path";

const repoRoot = resolve(new URL("..", import.meta.url).pathname);
const buildRoot = join(repoRoot, ".arduino-build");
const workspaceDir = join(repoRoot, "dist/arduino/AutoTyperWorkspace");
const workspaceHomeDir = join(workspaceDir, "home");
const workspaceDataDir = join(workspaceDir, "data");
const workspaceSketchbookDir = join(workspaceDir, "sketchbook");
const workspaceHardwareDir = join(workspaceSketchbookDir, "hardware");
const workspaceLibrariesDir = join(workspaceSketchbookDir, "libraries");
const workspaceSketchDir = join(workspaceSketchbookDir, "AutoTyperStaticSecrets");
const workspaceCliConfigPath = join(workspaceHomeDir, ".arduinoIDE/arduino-cli.yaml");
const workspaceLauncherPath = join(workspaceDir, "Launch AutoTyper Arduino.command");
const workspaceIdeAnchorPath = join(workspaceDir, "ARDUINO_IDE_ANCHOR.txt");
const sourceCliDataDir = join(repoRoot, "src/test/.arduino-cli/data");
const sourceEsp32HardwareDir = join(sourceCliDataDir, "packages/esp32/hardware/esp32/3.3.10");
const sourceAutoTyperCoreDir = join(repoRoot, "dist/arduino/AutoTyperCore");
const workspaceWrapperPlatformDir = join(workspaceHardwareDir, "autotyper/esp32");
const workspaceUpstreamPlatformDir = join(workspaceHardwareDir, "autotyper_upstream/esp32");
const workspaceFqbn = "autotyper:esp32:autotyper_esp32s3";

function run(command, args, options = {}) {
  return execFileSync(command, args, {
    cwd: repoRoot,
    encoding: options.capture ? "utf8" : undefined,
    stdio: options.capture ? ["ignore", "pipe", "inherit"] : "inherit",
  });
}

function ensureCleanDir(dirPath) {
  rmSync(dirPath, { recursive: true, force: true });
  mkdirSync(dirPath, { recursive: true });
}

function resolveArduinoIdeApp() {
  const candidates = [
    "/Applications/Arduino IDE.app",
    "/Applications/Arduino.app",
    `${process.env.HOME ?? ""}/Applications/Arduino IDE.app`,
    `${process.env.HOME ?? ""}/Applications/Arduino.app`,
  ];
  const match = candidates.find((candidate) => candidate && existsSync(candidate));
  if (!match) {
    throw new Error("Unable to locate a local Arduino IDE.app installation to bundle into the workspace.");
  }
  return match;
}

function writeWorkspaceCliConfig(rootDir) {
  mkdirSync(join(rootDir, "home/.arduinoIDE"), { recursive: true });
  mkdirSync(join(rootDir, "home/Library/Application Support/arduino-ide"), { recursive: true });

  writeFileSync(
    join(rootDir, "home/.arduinoIDE/arduino-cli.yaml"),
    [
      "directories:",
      `  data: ${join(rootDir, "data")}`,
      `  downloads: ${join(rootDir, "data/staging")}`,
      `  user: ${join(rootDir, "sketchbook")}`,
      "",
    ].join("\n"),
  );

  writeFileSync(
    join(rootDir, "home/Library/Application Support/arduino-ide/settings.json"),
    JSON.stringify(
      {
        "arduino.checkForUpdates": false,
        "arduino.enableTelemetry": false,
      },
      null,
      2,
    ),
  );
}

function wrapperBoardsTxt() {
  return [
    "autotyper_esp32s3.name=AutoTyper ESP32S3 Dev Module",
    "autotyper_esp32s3.bootloader.tool=esptool_py",
    "autotyper_esp32s3.bootloader.tool.default=esptool_py",
    "autotyper_esp32s3.upload.tool=esptool_py",
    "autotyper_esp32s3.upload.tool.default=esptool_py",
    "autotyper_esp32s3.upload.tool.network=esp_ota",
    "autotyper_esp32s3.upload.maximum_size=3145728",
    "autotyper_esp32s3.upload.maximum_data_size=327680",
    "autotyper_esp32s3.upload.speed=921600",
    "autotyper_esp32s3.upload.flags=",
    "autotyper_esp32s3.upload.extra_flags=",
    "autotyper_esp32s3.upload.use_1200bps_touch=false",
    "autotyper_esp32s3.upload.wait_for_upload_port=false",
    "autotyper_esp32s3.serial.disableDTR=false",
    "autotyper_esp32s3.serial.disableRTS=false",
    "autotyper_esp32s3.build.tarch=xtensa",
    "autotyper_esp32s3.build.bootloader_addr=0x0",
    "autotyper_esp32s3.build.target=esp32s3",
    "autotyper_esp32s3.build.mcu=esp32s3",
    "autotyper_esp32s3.build.core=autotyper_upstream:esp32",
    "autotyper_esp32s3.build.variant=autotyper_upstream:esp32s3",
    "autotyper_esp32s3.build.board=AUTOTYPER_ESP32S3_DEV",
    "autotyper_esp32s3.build.usb_mode=1",
    "autotyper_esp32s3.build.cdc_on_boot=0",
    "autotyper_esp32s3.build.msc_on_boot=0",
    "autotyper_esp32s3.build.dfu_on_boot=0",
    "autotyper_esp32s3.build.f_cpu=240000000L",
    "autotyper_esp32s3.build.flash_size=16MB",
    "autotyper_esp32s3.build.flash_freq=80m",
    "autotyper_esp32s3.build.flash_mode=dout",
    "autotyper_esp32s3.build.boot=opi",
    "autotyper_esp32s3.build.boot_freq=80m",
    "autotyper_esp32s3.build.partitions=app3M_fat9M_16MB",
    "autotyper_esp32s3.build.defines=-DBOARD_HAS_PSRAM",
    "autotyper_esp32s3.build.loop_core=-DARDUINO_RUNNING_CORE=1",
    "autotyper_esp32s3.build.event_core=-DARDUINO_EVENT_RUNNING_CORE=1",
    "autotyper_esp32s3.build.psram_type=opi",
    "autotyper_esp32s3.build.memory_type={build.boot}_{build.psram_type}",
    "",
  ].join("\n");
}

function wrapperPlatformTxt() {
  return [
    "name=AutoTyper ESP32 Wrapper",
    "version=0.1.0",
    "runtime.use_core_platform_path_for_runtime_platform_path=true",
    "",
  ].join("\n");
}

function starterSketch() {
  return [
    '#include <AutoTyperFirmware.h>',
    '#include "Secrets.h"',
    "",
    "namespace {",
    "",
    "const auto_typer::FirmwareConfig kFirmwareConfig = {",
    "    {",
    "        AUTO_TYPER_WIFI_SSID,",
    "        AUTO_TYPER_WIFI_PASSWORD,",
    "    },",
    "};",
    "",
    "}  // namespace",
    "",
    "void setup() {",
    "  auto_typer::autoTyperSetup(kFirmwareConfig);",
    "}",
    "",
    "void loop() {",
    "  auto_typer::autoTyperLoop();",
    "}",
    "",
  ].join("\n");
}

function starterSecretsTemplate() {
  return [
    "#pragma once",
    "",
    "// Edit these values before uploading the firmware.",
    '#define AUTO_TYPER_WIFI_SSID "your-wifi-ssid"',
    '#define AUTO_TYPER_WIFI_PASSWORD "your-wifi-password"',
    "",
  ].join("\n");
}

function workspaceReadme(ideAppPath) {
  return [
    "# AutoTyper Workspace",
    "",
    "This offline workspace is intended for internal AutoTyper firmware uploads.",
    "",
    `Expected local Arduino IDE: \`${ideAppPath}\``,
    "",
    "## Quick Start",
    "",
    "1. Open `Launch AutoTyper Arduino.command`.",
    "2. Edit `sketchbook/AutoTyperStaticSecrets/Secrets.h` with your Wi-Fi credentials.",
    "3. In Arduino IDE, select the serial port for the device.",
    "4. Use the only supported board entry: `AutoTyper ESP32S3 Dev Module`.",
    "5. Click Upload.",
    "",
    "The workspace ships with:",
    "- one visible AutoTyper board profile",
    "- the bundled `AutoTyperCore` library",
    "- the required ESP32 toolchain and support files",
    "- a starter sketch ready for Wi-Fi editing",
    "- a launcher that opens this workspace with the locally installed Arduino IDE",
    "",
    "After boot, the device prints its connected Wi-Fi IP to serial.",
    "",
  ].join("\n");
}

function workspaceLauncherScript() {
  return [
    "#!/bin/zsh",
    "set -euo pipefail",
    "",
    'WORKSPACE_DIR="$(cd "$(dirname "$0")" && pwd)"',
    'HOME_DIR="$WORKSPACE_DIR/home"',
    'SKETCH_PATH="$WORKSPACE_DIR/sketchbook/AutoTyperStaticSecrets/AutoTyperStaticSecrets.ino"',
    'APP_CANDIDATES=(',
    '  "/Applications/Arduino IDE.app"',
    '  "/Applications/Arduino.app"',
    '  "$HOME/Applications/Arduino IDE.app"',
    '  "$HOME/Applications/Arduino.app"',
    ')',
    'APP_PATH=""',
    'for candidate in "${APP_CANDIDATES[@]}"; do',
    '  if [[ -d "$candidate" ]]; then',
    '    APP_PATH="$candidate"',
    '    break',
    '  fi',
    'done',
    'if [[ -z "$APP_PATH" ]]; then',
    '  osascript -e \'display alert "Arduino IDE not found" message "Install Arduino IDE on this Mac, then re-run this launcher." as critical\'',
    '  exit 1',
    'fi',
    "",
    'mkdir -p "$HOME_DIR/.arduinoIDE" "$HOME_DIR/Library/Application Support/arduino-ide"',
    'cat > "$HOME_DIR/.arduinoIDE/arduino-cli.yaml" <<EOF',
    "directories:",
    '  data: "$WORKSPACE_DIR/data"',
    '  downloads: "$WORKSPACE_DIR/data/staging"',
    '  user: "$WORKSPACE_DIR/sketchbook"',
    "EOF",
    "",
    'export HOME="$HOME_DIR"',
    'exec "$APP_PATH/Contents/MacOS/Arduino IDE" "$SKETCH_PATH"',
    "",
  ].join("\n");
}

function packageWorkspace() {
  run("rtk", ["npm", "run", "firmware:package"]);

  ensureCleanDir(workspaceDir);
  mkdirSync(workspaceHomeDir, { recursive: true });
  mkdirSync(workspaceSketchbookDir, { recursive: true });
  mkdirSync(workspaceLibrariesDir, { recursive: true });
  mkdirSync(workspaceHardwareDir, { recursive: true });

  const ideAppPath = resolveArduinoIdeApp();
  cpSync(sourceCliDataDir, workspaceDataDir, { recursive: true });
  rmSync(join(workspaceDataDir, "packages/esp32/hardware"), { recursive: true, force: true });

  cpSync(sourceEsp32HardwareDir, workspaceUpstreamPlatformDir, { recursive: true });
  writeFileSync(join(workspaceUpstreamPlatformDir, "boards.txt"), "# hidden helper platform for AutoTyper workspace\n");

  mkdirSync(workspaceWrapperPlatformDir, { recursive: true });
  writeFileSync(join(workspaceWrapperPlatformDir, "boards.txt"), wrapperBoardsTxt());
  writeFileSync(join(workspaceWrapperPlatformDir, "platform.txt"), wrapperPlatformTxt());

  cpSync(sourceAutoTyperCoreDir, join(workspaceLibrariesDir, "AutoTyperCore"), { recursive: true });

  mkdirSync(workspaceSketchDir, { recursive: true });
  writeFileSync(join(workspaceSketchDir, "AutoTyperStaticSecrets.ino"), starterSketch());
  writeFileSync(join(workspaceSketchDir, "Secrets.h"), starterSecretsTemplate());
  writeFileSync(join(workspaceSketchDir, "Secrets.example.h"), starterSecretsTemplate());
  writeFileSync(join(workspaceDir, "README.md"), workspaceReadme(ideAppPath));
  writeFileSync(workspaceIdeAnchorPath, `${ideAppPath}\n`);

  writeWorkspaceCliConfig(workspaceDir);
  writeFileSync(workspaceLauncherPath, workspaceLauncherScript());
  chmodSync(workspaceLauncherPath, 0o755);
}

function verifyWorkspace() {
  packageWorkspace();

  assert.equal(existsSync(workspaceIdeAnchorPath), true, "Workspace must record the expected local Arduino IDE path");
  assert.equal(existsSync(join(workspaceLibrariesDir, "AutoTyperCore/src/esp32s3/libauto_typer_core.a")), true);
  assert.equal(existsSync(join(workspaceSketchDir, "Secrets.h")), true);
  assert.equal(existsSync(join(workspaceWrapperPlatformDir, "boards.txt")), true);
  assert.equal(existsSync(join(workspaceUpstreamPlatformDir, "platform.txt")), true);

  const boardList = run(
    "rtk",
    ["arduino-cli", "board", "listall", "--config-file", workspaceCliConfigPath],
    { capture: true },
  );
  const boardLines = boardList
    .split("\n")
    .map((line) => line.trim())
    .filter((line) => line.includes("ESP32S3 Dev Module"));
  assert.deepEqual(
    boardLines,
    ["AutoTyper ESP32S3 Dev Module autotyper:esp32:autotyper_esp32s3"],
    "Workspace board discovery must expose only the custom AutoTyper board",
  );

  const showProperties = run(
    "rtk",
    [
      "arduino-cli",
      "compile",
      "--config-file",
      workspaceCliConfigPath,
      "--build-path",
      join(buildRoot, "workspace-show-properties"),
      "--show-properties=expanded",
      "--fqbn",
      workspaceFqbn,
      workspaceSketchDir,
    ],
    { capture: true },
  );
  assert.match(showProperties, /^build\.flash_size=16MB$/m);
  assert.match(showProperties, /^build\.psram_type=opi$/m);
  assert.match(showProperties, /^build\.partitions=app3M_fat9M_16MB$/m);
  assert.match(showProperties, /^build\.core=autotyper_upstream:esp32$/m);
  assert.match(showProperties, /^build\.variant=autotyper_upstream:esp32s3$/m);

  run("rtk", [
    "arduino-cli",
    "compile",
    "--config-file",
    workspaceCliConfigPath,
    "--clean",
    "--build-path",
    join(buildRoot, "workspace-verify"),
    "--fqbn",
    workspaceFqbn,
    workspaceSketchDir,
  ]);
}

const command = process.argv[2];

switch (command) {
  case "package":
    packageWorkspace();
    break;
  case "verify":
    verifyWorkspace();
    break;
  default:
    throw new Error(`Unknown command ${JSON.stringify(command)}. Use one of: package, verify.`);
}
