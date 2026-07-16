const path = require("node:path");

module.exports = async function electronDist() {
  const electronBinaryPath = require("electron");
  return path.dirname(electronBinaryPath);
};
