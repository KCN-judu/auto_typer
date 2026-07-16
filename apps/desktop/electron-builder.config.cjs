const path = require("node:path");

module.exports = {
  appId: "com.autotyper.desktop",
  productName: "Auto Typer Control",
  electronVersion: "42.5.0",
  electronDist: path.resolve(__dirname, "..", "..", "node_modules", "electron", "dist"),
  directories: {
    output: "release",
    buildResources: "resources",
  },
  files: ["dist/**/*", "dist-electron/**/*", "package.json"],
  extraMetadata: {
    main: "dist-electron/apps/desktop/electron/main.js",
  },
  asar: true,
  mac: {
    category: "public.app-category.utilities",
    target: ["dmg", "zip"],
    icon: "resources/icon.icns",
    hardenedRuntime: false,
    gatekeeperAssess: false,
  },
  dmg: {
    sign: false,
  },
  win: {
    target: ["portable"],
    icon: "resources/icon.ico",
    signAndEditExecutable: false,
  },
  portable: {
    artifactName: "${productName}-${version}-${os}-${arch}-portable.${ext}",
  },
  artifactName: "${productName}-${version}-${os}-${arch}.${ext}",
};
