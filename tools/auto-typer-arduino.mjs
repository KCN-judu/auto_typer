import { cpSync, existsSync, mkdirSync, readdirSync, rmSync, writeFileSync } from "node:fs";
import { execFileSync } from "node:child_process";
import { join, resolve } from "node:path";

const repoRoot = resolve(new URL("..", import.meta.url).pathname);
const arduinoConfigPath = join(repoRoot, "src/test/arduino-cli.yaml");
const sourceFirmwareDir = join(repoRoot, "src/auto_typer");
const distLibraryDir = join(repoRoot, "dist/arduino/AutoTyperCore");
const distLibrarySrcDir = join(distLibraryDir, "src");
const distLibraryArchiveDir = join(distLibrarySrcDir, "esp32s3");
const buildRoot = join(repoRoot, ".arduino-build");
const targetFqbn =
  "esp32:esp32:esp32s3:FlashSize=16M,FlashMode=opi,PSRAM=opi,PartitionScheme=app3M_fat9M_16MB";

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

function escapeCString(value) {
  return value.replaceAll("\\", "\\\\").replaceAll('"', '\\"');
}

function resolveSecrets({ requireRealSecrets }) {
  const ssid = process.env.AUTO_TYPER_WIFI_SSID ?? "auto-typer-build-ssid";
  const password = process.env.AUTO_TYPER_WIFI_PASSWORD ?? "auto-typer-build-password";
  if (requireRealSecrets && (!process.env.AUTO_TYPER_WIFI_SSID || !process.env.AUTO_TYPER_WIFI_PASSWORD)) {
    throw new Error("Set AUTO_TYPER_WIFI_SSID and AUTO_TYPER_WIFI_PASSWORD before running upload.");
  }
  return { ssid, password };
}

function writeSecretsHeader(filePath, secrets) {
  writeFileSync(
    filePath,
    [
      "#pragma once",
      "",
      `#define AUTO_TYPER_WIFI_SSID "${escapeCString(secrets.ssid)}"`,
      `#define AUTO_TYPER_WIFI_PASSWORD "${escapeCString(secrets.password)}"`,
      "",
    ].join("\n"),
  );
}

function stageSourceSketch(secrets) {
  const sketchDir = join(buildRoot, "staging/source-sketch/auto_typer");
  ensureCleanDir(sketchDir);
  cpSync(sourceFirmwareDir, sketchDir, { recursive: true });
  mkdirSync(join(sketchDir, "config"), { recursive: true });
  writeSecretsHeader(join(sketchDir, "config/Secrets.h"), secrets);
  return sketchDir;
}

function stagePackageExampleSketch(secrets) {
  const sketchDir = join(buildRoot, "staging/package-example/AutoTyperStaticSecrets");
  ensureCleanDir(sketchDir);
  writeFileSync(join(sketchDir, "AutoTyperStaticSecrets.ino"), packageExampleSketch());
  writeFileSync(join(sketchDir, "Secrets.example.h"), packageSecretsExample());
  writeSecretsHeader(join(sketchDir, "Secrets.h"), secrets);
  return sketchDir;
}

function compileSketch(sketchDir, buildDir, extraArgs = []) {
  ensureCleanDir(buildDir);
  run("rtk", [
    "arduino-cli",
    "compile",
    "--config-file",
    arduinoConfigPath,
    "--clean",
    "--build-path",
    buildDir,
    "--fqbn",
    targetFqbn,
    ...extraArgs,
    sketchDir,
  ]);
}

function collectObjectFiles(dirPath) {
  const entries = readdirSync(dirPath, { withFileTypes: true });
  const objects = [];
  for (const entry of entries) {
    const nextPath = join(dirPath, entry.name);
    if (entry.isDirectory()) {
      objects.push(...collectObjectFiles(nextPath));
      continue;
    }
    if (entry.isFile() && entry.name.endsWith(".o")) {
      objects.push(nextPath);
    }
  }
  return objects;
}

function resolveArchiveTool(sketchDir) {
  const showPropsBuildDir = join(buildRoot, "show-properties");
  ensureCleanDir(showPropsBuildDir);
  const raw = run(
    "rtk",
    [
      "arduino-cli",
      "compile",
      "--config-file",
      arduinoConfigPath,
      "--build-path",
      showPropsBuildDir,
      "--show-properties=expanded",
      "--fqbn",
      targetFqbn,
      sketchDir,
    ],
    { capture: true },
  );
  const compilerPath = raw.match(/^compiler\.path=(.+)$/m)?.[1]?.trim();
  const archiveCmd = raw.match(/^compiler\.ar\.cmd=(.+)$/m)?.[1]?.trim();
  if (!compilerPath || !archiveCmd) {
    throw new Error("Unable to resolve the ESP32 archive tool from Arduino CLI build properties.");
  }
  return join(compilerPath, archiveCmd);
}

function sourceCompileOnly() {
  const secrets = resolveSecrets({ requireRealSecrets: false });
  const sketchDir = stageSourceSketch(secrets);
  compileSketch(sketchDir, join(buildRoot, "source-compile"));
}

function packageLibrary() {
  const secrets = resolveSecrets({ requireRealSecrets: false });
  const sourceSketchDir = stageSourceSketch(secrets);
  const sourceBuildDir = join(buildRoot, "source-compile");
  compileSketch(sourceSketchDir, sourceBuildDir);

  ensureCleanDir(distLibraryDir);
  mkdirSync(distLibrarySrcDir, { recursive: true });
  mkdirSync(distLibraryArchiveDir, { recursive: true });
  mkdirSync(join(distLibraryDir, "examples/AutoTyperStaticSecrets"), { recursive: true });

  cpSync(join(sourceFirmwareDir, "AutoTyperFirmware.h"), join(distLibrarySrcDir, "AutoTyperFirmware.h"));
  cpSync(join(sourceFirmwareDir, "AutoTyperConfig.h"), join(distLibrarySrcDir, "AutoTyperConfig.h"));
  writeFileSync(join(distLibraryDir, "library.properties"), packageLibraryProperties());
  writeFileSync(join(distLibraryDir, "README.md"), packageReadme());
  writeFileSync(join(distLibraryDir, "examples/AutoTyperStaticSecrets/AutoTyperStaticSecrets.ino"), packageExampleSketch());
  writeFileSync(join(distLibraryDir, "examples/AutoTyperStaticSecrets/Secrets.example.h"), packageSecretsExample());

  const sketchObjectDir = join(sourceBuildDir, "sketch");
  const sketchObjects = readdirSync(sketchObjectDir)
    .filter((name) => name.endsWith(".o") && name !== "auto_typer.ino.cpp.o")
    .map((name) => join(sketchObjectDir, name));
  const bundledLibraryObjects = collectObjectFiles(join(sourceBuildDir, "libraries"));
  const archiveMembers = [...sketchObjects, ...bundledLibraryObjects];
  if (archiveMembers.length === 0) {
    throw new Error("No firmware objects found to archive.");
  }

  run(resolveArchiveTool(sourceSketchDir), [
    "rcs",
    join(distLibraryArchiveDir, "libauto_typer_core.a"),
    ...archiveMembers,
  ]);
}

function verifyPackagedLibrary() {
  if (!existsSync(join(distLibraryArchiveDir, "libauto_typer_core.a"))) {
    throw new Error("Packaged library archive is missing. Run the package command first.");
  }
  const secrets = resolveSecrets({ requireRealSecrets: false });
  const sketchDir = stagePackageExampleSketch(secrets);
  compileSketch(sketchDir, join(buildRoot, "package-verify"), ["--library", distLibraryDir]);
}

function detectPort() {
  const raw = run("rtk", ["arduino-cli", "board", "list", "--format", "json", "--config-file", arduinoConfigPath], {
    capture: true,
  });
  const data = JSON.parse(raw);
  const ranked = (data.detected_ports ?? [])
    .map((entry) => entry.port)
    .filter((port) => typeof port?.address === "string")
    .sort((left, right) => scorePort(right.address) - scorePort(left.address));
  if (ranked.length === 0 || scorePort(ranked[0].address) <= 0) {
    throw new Error("No likely ESP32-S3 serial port detected. Pass --port=/dev/your-port.");
  }
  return ranked[0].address;
}

function scorePort(address) {
  if (address.includes("usbmodem")) return 4;
  if (address.includes("ttyACM")) return 3;
  if (address.includes("ttyUSB")) return 2;
  if (address.startsWith("/dev/cu.")) return 1;
  return 0;
}

function uploadPackagedLibrary() {
  packageLibrary();
  const secrets = resolveSecrets({ requireRealSecrets: true });
  const sketchDir = stagePackageExampleSketch(secrets);
  const portArg = process.argv.slice(3).find((arg) => arg.startsWith("--port="));
  const port = portArg ? portArg.slice("--port=".length) : detectPort();
  compileSketch(sketchDir, join(buildRoot, "package-upload"), ["--library", distLibraryDir, "--upload", "--port", port]);
}

function packageLibraryProperties() {
  return [
    "name=AutoTyperCore",
    "version=0.1.0",
    "author=Auto Typer",
    "maintainer=Auto Typer",
    "sentence=Precompiled ESP32-S3 Auto Typer firmware core.",
    "paragraph=Static-library distribution of the Auto Typer firmware core for ESP32S3 Dev Module 16MB OPI PSRAM targets.",
    "category=Device Control",
    "architectures=esp32",
    "includes=AutoTyperFirmware.h,AutoTyperConfig.h",
    "dot_a_linkage=true",
    "precompiled=full",
    "",
  ].join("\n");
}

function packageExampleSketch() {
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

function packageSecretsExample() {
  return [
    "#pragma once",
    "",
    '#define AUTO_TYPER_WIFI_SSID "your-wifi-ssid"',
    '#define AUTO_TYPER_WIFI_PASSWORD "your-wifi-password"',
    "",
  ].join("\n");
}

function packageReadme() {
  return [
    "# AutoTyperCore",
    "",
    "Precompiled Auto Typer firmware core for Arduino IDE and Arduino CLI.",
    "",
    "## Arduino IDE",
    "",
    "1. Install this folder as a library, or copy `AutoTyperCore` into your Arduino libraries directory.",
    "2. Open `examples/AutoTyperStaticSecrets/AutoTyperStaticSecrets.ino`.",
    "3. Copy `Secrets.example.h` to `Secrets.h` next to the sketch and fill in your Wi-Fi credentials.",
    "4. The packaged archive already bundles the required Adafruit and ArduinoJson library objects.",
    "5. Select these board options:",
    "   - Board: `ESP32S3 Dev Module`",
    "   - Flash Size: `16MB (128Mb)`",
    "   - Flash Mode: `OPI 80MHz`",
    "   - PSRAM: `OPI PSRAM`",
    "   - Partition Scheme: `16M Flash (3MB APP/9.9MB FATFS)`",
    "",
    "## Arduino CLI",
    "",
    "```bash",
    "arduino-cli compile \\",
    "  --fqbn esp32:esp32:esp32s3:FlashSize=16M,FlashMode=opi,PSRAM=opi,PartitionScheme=app3M_fat9M_16MB \\",
    "  --library /path/to/AutoTyperCore \\",
    "  /path/to/AutoTyperStaticSecrets",
    "```",
    "",
  ].join("\n");
}

const command = process.argv[2];

switch (command) {
  case "compile-source":
    sourceCompileOnly();
    break;
  case "package":
    packageLibrary();
    break;
  case "verify-package":
    verifyPackagedLibrary();
    break;
  case "upload-package":
    uploadPackagedLibrary();
    break;
  default:
    throw new Error(
      `Unknown command ${JSON.stringify(command)}. Use one of: compile-source, package, verify-package, upload-package.`,
    );
}
