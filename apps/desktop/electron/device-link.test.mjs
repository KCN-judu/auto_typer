import assert from "node:assert/strict";
import net from "node:net";
import { setTimeout as delay } from "node:timers/promises";
import { DeviceLink } from "../dist-electron/apps/desktop/electron/device-link.js";

const host = "127.0.0.1";

function createGroup() {
  return {
    v: 1,
    requestId: "group-1",
    type: "exec_group",
    groupId: "g-1",
    seq: 1,
    policy: { maxRuntimeMs: 5000, onDisconnect: "cancel" },
    blocks: [{ type: "wait", durationMs: 1 }],
  };
}

function createGroupWithId(requestId, groupId, seq = 1) {
  return {
    ...createGroup(),
    requestId,
    groupId,
    seq,
  };
}

async function withServer(handler, run) {
  const sockets = new Set();
  const server = net.createServer((socket) => {
    sockets.add(socket);
    socket.setNoDelay(true);
    let buffer = "";
    socket.on("data", (chunk) => {
      buffer += chunk.toString("utf8");
      while (true) {
        const newline = buffer.indexOf("\n");
        if (newline < 0) {
          return;
        }
        const line = buffer.slice(0, newline);
        buffer = buffer.slice(newline + 1);
        if (line.trim()) {
          handler(JSON.parse(line), socket);
        }
      }
    });
    socket.on("close", () => sockets.delete(socket));
  });
  await new Promise((resolve) => server.listen(0, host, resolve));
  const port = server.address().port;
  try {
    await run(port);
  } finally {
    for (const socket of sockets) {
      socket.destroy();
    }
    await new Promise((resolve) => server.close(resolve));
  }
}

function write(socket, message) {
  socket.write(`${JSON.stringify(message)}\n`);
}

async function connectLink(port) {
  const link = new DeviceLink();
  await link.connect(host, port);
  return link;
}

async function testDelayedAcceptedTimesOutWithoutResend() {
  let execCount = 0;
  await withServer((message, socket) => {
    if (message.type === "hello") {
      write(socket, { v: 1, type: "hello_ack", requestId: message.requestId, device: "auto_typer", protocol: "tcp_ndjson", capabilities: [], limits: { maxBlocksPerGroup: 32, maxMessageBytes: 8192, maxGroupRuntimeMs: 30000 } });
    }
    if (message.type === "exec_group") {
      execCount += 1;
      setTimeout(() => write(socket, { v: 1, type: "group_accepted", requestId: message.requestId, groupId: message.groupId, seq: message.seq, blockCount: message.blocks.length }), 80);
    }
    if (message.type === "ping") {
      write(socket, { v: 1, type: "pong", requestId: message.requestId });
    }
  }, async (port) => {
    const link = await connectLink(port);
    await assert.rejects(() => link.sendCommand(createGroup(), 30), /submit_timeout/);
    await delay(120);
    assert.equal(execCount, 1, "exec_group must not be resent after admission timeout");
    assert.equal(link.state(), "desync_pending");
    link.close();
  });
}

async function testLateStartedAfterSubmitTimeoutIsDesync() {
  const seen = [];
  await withServer((message, socket) => {
    if (message.type === "hello") {
      write(socket, { v: 1, type: "hello_ack", requestId: message.requestId, device: "auto_typer", protocol: "tcp_ndjson", capabilities: [], limits: { maxBlocksPerGroup: 32, maxMessageBytes: 8192, maxGroupRuntimeMs: 30000 } });
    }
    if (message.type === "exec_group") {
      setTimeout(() => write(socket, { v: 1, type: "group_started", groupId: message.groupId, seq: message.seq }), 70);
    }
    if (message.type === "ping") {
      write(socket, { v: 1, type: "pong", requestId: message.requestId });
    }
  }, async (port) => {
    const link = await connectLink(port);
    link.on("message", (message) => seen.push(message.type));
    await assert.rejects(() => link.sendCommand(createGroup(), 25), /submit_timeout/);
    await delay(100);
    assert.deepEqual(seen.filter((type) => type === "group_started"), ["group_started"]);
    assert.equal(link.state(), "desync_pending", "late execution event must not convert timed-out submit into success");
    link.close();
  });
}

async function testCancelDoesNotSpam() {
  let cancelCount = 0;
  await withServer((message, socket) => {
    if (message.type === "hello") {
      write(socket, { v: 1, type: "hello_ack", requestId: message.requestId, device: "auto_typer", protocol: "tcp_ndjson", capabilities: [], limits: { maxBlocksPerGroup: 32, maxMessageBytes: 8192, maxGroupRuntimeMs: 30000 } });
    }
    if (message.type === "exec_group") {
      write(socket, { v: 1, type: "group_accepted", requestId: message.requestId, groupId: message.groupId, seq: message.seq, blockCount: message.blocks.length });
    }
    if (message.type === "cancel") {
      cancelCount += 1;
      write(socket, { v: 1, type: "cancel_result", requestId: message.requestId, ok: true });
    }
    if (message.type === "ping") {
      write(socket, { v: 1, type: "pong", requestId: message.requestId });
    }
  }, async (port) => {
    const link = await connectLink(port);
    await link.sendCommand(createGroup(), 100);
    const first = await link.sendCommand({ v: 1, requestId: "cancel-1", type: "cancel" }, 100);
    const second = await link.sendCommand({ v: 1, requestId: "cancel-2", type: "cancel" }, 100);
    assert.equal(first.type, "cancel_result");
    assert.equal(second.type, "cancel_result");
    assert.equal(cancelCount, 1, "only one cancel should be written for an already-cancelling active group");
    link.close();
  });
}

async function testPingRequiresRealPong() {
  await withServer((message, socket) => {
    if (message.type === "hello") {
      write(socket, { v: 1, type: "hello_ack", requestId: message.requestId, device: "auto_typer", protocol: "tcp_ndjson", capabilities: [], limits: { maxBlocksPerGroup: 32, maxMessageBytes: 8192, maxGroupRuntimeMs: 30000 } });
    }
  }, async (port) => {
    const link = await connectLink(port);
    await assert.rejects(() => link.sendCommand({ v: 1, requestId: "ping-1", type: "ping" }, 30), /request_timeout/);
    link.close();
  });

  await withServer((message, socket) => {
    if (message.type === "hello") {
      write(socket, { v: 1, type: "hello_ack", requestId: message.requestId, device: "auto_typer", protocol: "tcp_ndjson", capabilities: [], limits: { maxBlocksPerGroup: 32, maxMessageBytes: 8192, maxGroupRuntimeMs: 30000 } });
    }
    if (message.type === "ping") {
      write(socket, { v: 1, type: "pong", requestId: message.requestId });
    }
  }, async (port) => {
    const link = await connectLink(port);
    const pong = await link.sendCommand({ v: 1, requestId: "ping-2", type: "ping" }, 100);
    assert.equal(pong.type, "pong");
    link.close();
  });
}

async function testGroupDoneClearsActiveGroup() {
  await withServer((message, socket) => {
    if (message.type === "hello") {
      write(socket, { v: 1, type: "hello_ack", requestId: message.requestId, device: "auto_typer", protocol: "tcp_ndjson", capabilities: [], limits: { maxBlocksPerGroup: 32, maxMessageBytes: 8192, maxGroupRuntimeMs: 30000 } });
    }
    if (message.type === "exec_group") {
      write(socket, { v: 1, type: "group_accepted", requestId: message.requestId, groupId: message.groupId, seq: message.seq, blockCount: message.blocks.length });
      setTimeout(() => write(socket, { v: 1, type: "group_done", groupId: message.groupId, seq: message.seq, ok: true, durationMs: 1 }), 10);
    }
    if (message.type === "ping") {
      write(socket, { v: 1, type: "pong", requestId: message.requestId });
    }
  }, async (port) => {
    const link = await connectLink(port);
    await link.sendCommand(createGroupWithId("group-1", "g-1"), 100);
    await delay(30);
    assert.equal(link.currentActiveGroup(), undefined, "group_done must release the active group for old firmware");
    await link.sendCommand(createGroupWithId("group-2", "g-2", 2), 100);
    link.close();
  });
}

async function testFaultWithGroupIdClearsActiveGroup() {
  await withServer((message, socket) => {
    if (message.type === "hello") {
      write(socket, { v: 1, type: "hello_ack", requestId: message.requestId, device: "auto_typer", protocol: "tcp_ndjson", capabilities: [], limits: { maxBlocksPerGroup: 32, maxMessageBytes: 8192, maxGroupRuntimeMs: 30000 } });
    }
    if (message.type === "exec_group") {
      write(socket, { v: 1, type: "group_accepted", requestId: message.requestId, groupId: message.groupId, seq: message.seq, blockCount: message.blocks.length });
      setTimeout(() => write(socket, { v: 1, type: "fault", groupId: message.groupId, seq: message.seq, code: "motion_feedback_timeout", message: "Motion feedback timed out" }), 10);
    }
    if (message.type === "ping") {
      write(socket, { v: 1, type: "pong", requestId: message.requestId });
    }
  }, async (port) => {
    const link = await connectLink(port);
    await link.sendCommand(createGroupWithId("group-1", "g-1"), 100);
    await delay(30);
    assert.equal(link.currentActiveGroup(), undefined, "matching fault must release the active group");
    link.close();
  });
}

await testDelayedAcceptedTimesOutWithoutResend();
await testLateStartedAfterSubmitTimeoutIsDesync();
await testCancelDoesNotSpam();
await testPingRequiresRealPong();
await testGroupDoneClearsActiveGroup();
await testFaultWithGroupIdClearsActiveGroup();

console.log("device-link TCP boundary tests passed");
