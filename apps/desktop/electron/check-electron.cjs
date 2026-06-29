const fs = require("node:fs");

let electronPath = "";
try {
  electronPath = require("electron");
} catch (error) {
  console.error("Electron package is not usable.");
  console.error(error instanceof Error ? error.message : String(error));
  process.exit(1);
}

if (typeof electronPath !== "string" || !fs.existsSync(electronPath)) {
  console.error("Electron binary is missing.");
  console.error("Run: rm -rf node_modules/electron && npm install");
  process.exit(1);
}

console.log(`Electron binary: ${electronPath}`);
