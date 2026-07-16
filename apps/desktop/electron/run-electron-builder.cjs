#!/usr/bin/env node

const { spawn } = require("node:child_process");

const defaultMirrorEnv = {
  ELECTRON_BUILDER_BINARIES_MIRROR: "https://npmmirror.com/mirrors/electron-builder-binaries/",
  ELECTRON_MIRROR: "https://npmmirror.com/mirrors/electron/",
};

const env = { ...process.env };

for (const [key, value] of Object.entries(defaultMirrorEnv)) {
  if (!env[key]) {
    env[key] = value;
  }
}

const child = spawn(
  process.execPath,
  [require.resolve("electron-builder/cli.js"), ...process.argv.slice(2)],
  {
    stdio: "inherit",
    env,
  }
);

child.on("exit", (code, signal) => {
  if (signal) {
    process.kill(process.pid, signal);
    return;
  }
  process.exit(code ?? 1);
});

child.on("error", (error) => {
  console.error(error);
  process.exit(1);
});
