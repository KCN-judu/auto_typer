import assert from "node:assert/strict";
import { SerialWifiLink } from "../dist-electron/apps/desktop/electron/serial-wifi-link.js";

function createLink() {
  const logs = [];
  const link = new SerialWifiLink((event) => logs.push(event));
  return { link, logs };
}

function waitForProtocolFrame(link, requestId) {
  return new Promise((resolve, reject) => {
    link.pending.set(requestId, { requestId, resolve, reject });
  });
}

function waitForScanFrame(link, requestId) {
  return new Promise((resolve, reject) => {
    link.pending.set(requestId, { requestId, resolve, reject, networks: [] });
  });
}

function protocolFrame(message) {
  return `ATWIFI<${JSON.stringify(message)}`;
}

function wifiStatusFrame(requestId) {
  return protocolFrame({
    v: 1,
    type: "wifi_status",
    requestId,
    wifi: {
      setupApActive: false,
      setupSsid: "",
      setupPassword: "",
      setupIpAddress: "",
      staConnected: true,
      staConnecting: false,
      staSsid: "CMCC-102",
      ipAddress: "192.168.10.144",
      wifiRssi: -77,
      savedCredentials: true,
      phase: "connected",
    },
  });
}

async function testInvalidProtocolLineDoesNotBlockNextFrame() {
  const { link, logs } = createLink();
  const response = waitForProtocolFrame(link, "wifi-ok");

  link.pushData("[serial-wifi] boot log\nATWIFI<not-json\n");
  link.pushData(`${wifiStatusFrame("wifi-ok")}\n`);

  const message = await response;
  assert.equal(message.type, "wifi_status");
  assert.equal(message.requestId, "wifi-ok");
  assert.equal(message.wifi.ipAddress, "192.168.10.144");
  assert.equal(logs.some((event) => event.protocol && event.line.startsWith("ATWIFI<not-json")), true);
}

async function testInvalidProtocolFrameRejectsMatchingPendingRequest() {
  const { link } = createLink();
  const response = waitForProtocolFrame(link, "scan-bad");

  link.pushData('ATWIFI<{"v":1,"type":"wifi_networks","requestId":"scan-bad","networks":oops}\n');

  await assert.rejects(response, /serial_invalid_frame/);
}

async function testSilentCloseResolvesPendingWithoutDisconnectError() {
  const { link } = createLink();
  const response = waitForProtocolFrame(link, "wifi-close");

  await link.close({ rejectPending: false });

  const message = await response;
  assert.equal(message.type, "protocol_error");
  assert.equal(message.requestId, "wifi-close");
  assert.equal(message.code, "serial_closed");
}

async function testScanWifiCollectsStreamedNetworkFrames() {
  const { link } = createLink();
  const response = waitForScanFrame(link, "scan-stream");

  link.pushData(`${protocolFrame({ v: 1, type: "wifi_network", requestId: "scan-stream", network: { ssid: "CMCC-102", rssi: -71, channel: 1, encryption: "wpa_wpa2" } })}\n`);
  link.pushData(`${protocolFrame({ v: 1, type: "wifi_network", requestId: "scan-stream", network: { ssid: "Phone Hotspot", rssi: -42, channel: 6, encryption: "wpa2" } })}\n`);
  link.pushData(`${protocolFrame({ v: 1, type: "wifi_networks", requestId: "scan-stream", ok: true, networks: [] })}\n`);

  const message = await response;
  assert.equal(message.type, "wifi_networks");
  assert.deepEqual(message.networks.map((network) => network.ssid), ["CMCC-102", "Phone Hotspot"]);
}

await testInvalidProtocolLineDoesNotBlockNextFrame();
await testInvalidProtocolFrameRejectsMatchingPendingRequest();
await testSilentCloseResolvesPendingWithoutDisconnectError();
await testScanWifiCollectsStreamedNetworkFrames();

console.log("serial-wifi-link frame recovery tests passed");
