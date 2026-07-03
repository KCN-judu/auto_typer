import assert from "node:assert/strict";
import { SerialWifiLink } from "../dist-electron/apps/desktop/electron/serial-wifi-link.js";

function createLink() {
  const logs = [];
  const link = new SerialWifiLink((event) => logs.push(event));
  return { link, logs };
}

function waitForProtocolFrame(link, requestId) {
  return new Promise((resolve, reject) => {
    link.pending.set(requestId, { requestId, commandType: "get_wifi_status", resolve, reject });
  });
}

function waitForScanFrame(link, requestId) {
  return new Promise((resolve, reject) => {
    link.pending.set(requestId, { requestId, commandType: "scan_wifi", resolve, reject, networks: [], scanStarted: false });
  });
}

async function startMockedScan(link) {
  const writes = [];
  link.writeStream = {
    destroyed: false,
    write(line, _encoding, callback) {
      writes.push(line);
      callback?.();
      return true;
    },
  };
  const response = link.scanWifi();
  await new Promise((resolve) => setImmediate(resolve));
  assert.equal(writes.length, 1);
  const command = JSON.parse(writes[0].slice("ATWIFI>".length));
  assert.equal(command.type, "scan_wifi");
  return { response, requestId: command.requestId };
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
  const { response, requestId } = await startMockedScan(link);

  link.pushData(`${protocolFrame({ v: 1, type: "wifi_scan_started", requestId })}\n`);
  link.pushData(`${protocolFrame({ v: 1, type: "wifi_network", requestId, network: { ssid: "CMCC-102", rssi: -71, channel: 1, encryption: "wpa_wpa2" } })}\n`);
  link.pushData(`${protocolFrame({ v: 1, type: "wifi_network", requestId, network: { ssid: "Phone Hotspot", rssi: -42, channel: 6, encryption: "wpa2" } })}\n`);
  link.pushData(`${protocolFrame({ v: 1, type: "wifi_networks", requestId, ok: true, count: 2, networks: [] })}\n`);

  const message = await response;
  assert.equal(message.type, "wifi_networks");
  assert.deepEqual(message.networks.map((network) => network.ssid), ["Phone Hotspot", "CMCC-102"]);
  assert.equal(message.count, 2);
  assert.equal(link.pending.has(requestId), false);
}

async function testScanStartedIsNonTerminalProgress() {
  const { link, logs } = createLink();
  const { response, requestId } = await startMockedScan(link);
  let resolved = false;
  response.then(() => {
    resolved = true;
  });

  link.pushData(`${protocolFrame({ v: 1, type: "wifi_scan_started", requestId })}\n`);
  await new Promise((resolve) => setImmediate(resolve));

  assert.equal(resolved, false);
  assert.equal(link.pending.has(requestId), true);
  assert.equal(logs.some((event) => event.line.includes(`scan started requestId=${requestId}`)), true);

  link.pushData(`${protocolFrame({ v: 1, type: "wifi_networks", requestId, ok: true, count: 0, networks: [] })}\n`);
  const message = await response;
  assert.equal(message.type, "wifi_networks");
}

async function testLateWifiNetworkAfterTerminalDoesNotMutateResult() {
  const { link, logs } = createLink();
  const { response, requestId } = await startMockedScan(link);

  link.pushData(`${protocolFrame({ v: 1, type: "wifi_network", requestId, network: { ssid: "Before Final", rssi: -60, channel: 11, encryption: "wpa2" } })}\n`);
  link.pushData(`${protocolFrame({ v: 1, type: "wifi_networks", requestId, ok: true, count: 1, networks: [] })}\n`);

  const message = await response;
  assert.deepEqual(message.networks.map((network) => network.ssid), ["Before Final"]);
  assert.equal(message.count, 1);
  assert.equal(link.pending.has(requestId), false);

  link.pushData(`${protocolFrame({ v: 1, type: "wifi_network", requestId, network: { ssid: "After Final", rssi: -50, channel: 1, encryption: "wpa2" } })}\n`);

  assert.deepEqual(message.networks.map((network) => network.ssid), ["Before Final"]);
  assert.equal(logs.some((event) => event.line.includes(`rx frame type=wifi_network requestId=${requestId} pending=0`)), true);
}

async function testEmptyScanFinalResolvesSuccessfully() {
  const { link } = createLink();
  const { response, requestId } = await startMockedScan(link);

  link.pushData(`${protocolFrame({ v: 1, type: "wifi_scan_started", requestId })}\n`);
  link.pushData(`${protocolFrame({ v: 1, type: "wifi_networks", requestId, ok: true, count: 0, networks: [] })}\n`);

  const message = await response;
  assert.equal(message.type, "wifi_networks");
  assert.equal(message.ok, true);
  assert.deepEqual(message.networks, []);
}

await testInvalidProtocolLineDoesNotBlockNextFrame();
await testInvalidProtocolFrameRejectsMatchingPendingRequest();
await testSilentCloseResolvesPendingWithoutDisconnectError();
await testScanWifiCollectsStreamedNetworkFrames();
await testScanStartedIsNonTerminalProgress();
await testLateWifiNetworkAfterTerminalDoesNotMutateResult();
await testEmptyScanFinalResolvesSuccessfully();

console.log("serial-wifi-link frame recovery tests passed");
