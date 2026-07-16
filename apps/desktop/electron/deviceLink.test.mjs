import assert from "node:assert/strict";
import net from "node:net";
import { setTimeout as delay } from "node:timers/promises";
import { DeviceLink } from "../dist-electron/apps/desktop/electron/deviceLink.js";

const host = "127.0.0.1";

function createBlock(requestId = "block-1", blockId = "b-1", seq = 1) {
  return {
    v: 1,
    requestId,
    type: "execute_block",
    blockId,
    seq,
    policy: { maxRuntimeMs: 5000, onDisconnect: "cancel" },
    block: [{ type: "wait", durationMs: 1 }],
  };
}

async function withServer(handler, run) {
  const sockets = new Set();
  const server = net.createServer((socket) => {
    sockets.add(socket);
    let buffer = "";
    socket.on("data", (chunk) => {
      buffer += chunk.toString("utf8");
      while (buffer.includes("\n")) {
        const newline = buffer.indexOf("\n");
        const line = buffer.slice(0, newline);
        buffer = buffer.slice(newline + 1);
        if (line.trim()) handler(JSON.parse(line), socket);
      }
    });
    socket.on("close", () => sockets.delete(socket));
  });
  await new Promise((resolve) => server.listen(0, host, resolve));
  try {
    await run(server.address().port);
  } finally {
    for (const socket of sockets) socket.destroy();
    await new Promise((resolve) => server.close(resolve));
  }
}

function write(socket, message) {
  socket.write(`${JSON.stringify(message)}\n`);
}

function handleSessionMessage(message, socket) {
  if (message.type === "handshake") {
    write(socket, {
      v: 1,
      type: "handshake_ack",
      requestId: message.requestId,
      device: "auto_typer",
      protocol: "tcp_ndjson",
      capabilities: ["execute_block"],
      limits: { maxMessageBytes: 8192, maxBlockRuntimeMs: 30000, maxActionTimeoutMs: 10000 },
    });
  } else if (message.type === "heartbeat") {
    write(socket, { v: 1, type: "heartbeat_ack", requestId: message.requestId });
  }
}

async function connectLink(port) {
  const link = new DeviceLink();
  await link.connect(host, port);
  return link;
}

async function testAckTimeoutDoesNotResend() {
  let executeCount = 0;
  await withServer((message, socket) => {
    handleSessionMessage(message, socket);
    if (message.type === "execute_block") {
      executeCount += 1;
      setTimeout(() => write(socket, { v: 1, type: "block_ack", requestId: message.requestId, blockId: message.blockId, seq: message.seq }), 80);
    }
  }, async (port) => {
    const link = await connectLink(port);
    await assert.rejects(() => link.sendCommand(createBlock(), 30), /block_ack_timeout/);
    await delay(120);
    assert.equal(executeCount, 1);
    assert.equal(link.state(), "uncertain");
    assert.deepEqual(link.currentActiveBlock(), { blockId: "b-1", seq: 1 });
    link.close();
  });
}

async function testImmediateResultDoesNotLeaveStaleActiveBlock() {
  await withServer((message, socket) => {
    handleSessionMessage(message, socket);
    if (message.type === "execute_block") {
      write(socket, { v: 1, type: "block_ack", requestId: message.requestId, blockId: message.blockId, seq: message.seq });
      write(socket, { v: 1, type: "block_result", blockId: message.blockId, seq: message.seq, status: "done" });
    }
  }, async (port) => {
    const link = await connectLink(port);
    await link.sendCommand(createBlock(), 100);
    await delay(10);
    assert.equal(link.currentActiveBlock(), undefined);
    link.close();
  });
}

async function testProtocolRejectionReleasesPendingBlock() {
  await withServer((message, socket) => {
    handleSessionMessage(message, socket);
    if (message.type === "execute_block") {
      write(socket, { v: 1, type: "protocol_error", requestId: message.requestId, code: "invalid_block", message: "invalid block" });
    }
  }, async (port) => {
    const link = await connectLink(port);
    const response = await link.sendCommand(createBlock(), 100);
    assert.equal(response.type, "protocol_error");
    assert.equal(link.currentActiveBlock(), undefined);
    assert.equal(link.state(), "connected");
    link.close();
  });
}

async function testOnlyResultReleasesBlock() {
  await withServer((message, socket) => {
    handleSessionMessage(message, socket);
    if (message.type === "execute_block") {
      write(socket, { v: 1, type: "block_ack", requestId: message.requestId, blockId: message.blockId, seq: message.seq });
    }
  }, async (port) => {
    const link = await connectLink(port);
    await link.sendCommand(createBlock(), 100);
    assert.deepEqual(link.currentActiveBlock(), { blockId: "b-1", seq: 1 });
    await assert.rejects(() => link.sendCommand(createBlock("block-2", "b-2", 2), 100), /device_busy/);
    link.close();
  });
}

async function testMatchingResultAllowsNextBlock() {
  await withServer((message, socket) => {
    handleSessionMessage(message, socket);
    if (message.type === "execute_block") {
      write(socket, { v: 1, type: "block_ack", requestId: message.requestId, blockId: message.blockId, seq: message.seq });
      setTimeout(() => write(socket, { v: 1, type: "block_result", blockId: message.blockId, seq: message.seq, status: "done" }), 10);
    }
  }, async (port) => {
    const link = await connectLink(port);
    await link.sendCommand(createBlock(), 100);
    await delay(30);
    assert.equal(link.currentActiveBlock(), undefined);
    await link.sendCommand(createBlock("block-2", "b-2", 2), 100);
    link.close();
  });
}

async function testFaultReleasesBlock() {
  await withServer((message, socket) => {
    handleSessionMessage(message, socket);
    if (message.type === "execute_block") {
      write(socket, { v: 1, type: "block_ack", requestId: message.requestId, blockId: message.blockId, seq: message.seq });
      setTimeout(() => write(socket, { v: 1, type: "fault", code: "transport_fault", message: "motion transport failed" }), 10);
    }
  }, async (port) => {
    const link = await connectLink(port);
    await link.sendCommand(createBlock(), 100);
    await delay(30);
    assert.equal(link.currentActiveBlock(), undefined);
    assert.equal(link.state(), "uncertain");
    link.close();
  });
}

async function testFinishTaskRequiresEmptyBlockQueue() {
  let finishCount = 0;
  await withServer((message, socket) => {
    handleSessionMessage(message, socket);
    if (message.type === "execute_block") {
      write(socket, { v: 1, type: "block_ack", requestId: message.requestId, blockId: message.blockId, seq: message.seq });
    } else if (message.type === "finish_task") {
      finishCount += 1;
      write(socket, { v: 1, type: "finish_task_result", requestId: message.requestId, ok: true });
    }
  }, async (port) => {
    const link = await connectLink(port);
    await link.sendCommand(createBlock(), 100);
    await assert.rejects(
      () => link.sendCommand({ v: 1, requestId: "finish-1", type: "finish_task" }, 100),
      /device_busy/,
    );
    assert.equal(finishCount, 0, "finish_task must not be written while a block is active");
    link.close();
  });
}

async function testEmergencyStopBypassesActiveBlock() {
  let emergencyCount = 0;
  await withServer((message, socket) => {
    handleSessionMessage(message, socket);
    if (message.type === "execute_block") {
      write(socket, { v: 1, type: "block_ack", requestId: message.requestId, blockId: message.blockId, seq: message.seq });
    } else if (message.type === "emergency_stop") {
      emergencyCount += 1;
      write(socket, { v: 1, type: "emergency_stop_result", requestId: message.requestId, ok: true });
    }
  }, async (port) => {
    const link = await connectLink(port);
    await link.sendCommand(createBlock(), 100);
    const response = await link.sendCommand({ v: 1, requestId: "stop-1", type: "emergency_stop" }, 100);
    assert.equal(response.type, "emergency_stop_result");
    assert.equal(emergencyCount, 1);
    assert.equal(link.currentActiveBlock(), undefined);
    assert.equal(link.state(), "uncertain");
    link.close();
  });
}

await testAckTimeoutDoesNotResend();
await testImmediateResultDoesNotLeaveStaleActiveBlock();
await testProtocolRejectionReleasesPendingBlock();
await testOnlyResultReleasesBlock();
await testMatchingResultAllowsNextBlock();
await testFaultReleasesBlock();
await testFinishTaskRequiresEmptyBlockQueue();
await testEmergencyStopBypassesActiveBlock();

console.log("deviceLink TCP boundary tests passed");
