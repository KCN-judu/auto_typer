import net from "node:net";

const connectTimeoutMs = 5000;
const helloTimeoutMs = 3000;
const writeTimeoutMs = 2000;

const host = process.argv[2];
const port = Number(process.argv[3] ?? 7777);

if (!host || !Number.isFinite(port)) {
  console.error("usage: npm run tcp:probe -- <host> <port>");
  process.exit(2);
}

const socket = new net.Socket();
let buffer = "";
let sawHelloAck = false;
let exitTimer;
let helloTimer;

function fail(code, detail = "") {
  if (exitTimer) {
    clearTimeout(exitTimer);
  }
  if (helloTimer) {
    clearTimeout(helloTimer);
  }
  console.error(detail ? `${code}: ${detail}` : code);
  socket.destroy();
  process.exitCode = 1;
  setTimeout(() => process.exit(1), 10).unref();
}

const connectTimer = setTimeout(() => fail("connect_timeout"), connectTimeoutMs);

function writeLineAsync(payload) {
  return new Promise((resolve, reject) => {
    let settled = false;
    let writeCallbackDone = false;
    let drainDone = false;
    const timer = setTimeout(() => finish(new Error("write_timeout")), writeTimeoutMs);
    const finish = (error) => {
      if (settled) {
        return;
      }
      settled = true;
      clearTimeout(timer);
      socket.off("error", onError);
      socket.off("close", onClose);
      socket.off("drain", onDrain);
      if (error) {
        reject(error);
      } else {
        resolve();
      }
    };
    const maybeFinish = () => {
      if (writeCallbackDone && drainDone) {
        finish();
      }
    };
    const onError = (error) => finish(error);
    const onClose = () => finish(new Error("disconnected"));
    const onDrain = () => {
      drainDone = true;
      maybeFinish();
    };
    socket.once("error", onError);
    socket.once("close", onClose);
    const flushed = socket.write(payload, "utf8", () => {
      writeCallbackDone = true;
      maybeFinish();
    });
    drainDone = flushed;
    if (!flushed) {
      socket.once("drain", onDrain);
    }
  });
}

function handleLine(line) {
  console.log(line);
  let message;
  try {
    message = JSON.parse(line);
  } catch {
    fail("invalid_json", line);
    return;
  }
  if (message.type === "hello_ack") {
    sawHelloAck = true;
    if (helloTimer) {
      clearTimeout(helloTimer);
    }
    exitTimer = setTimeout(() => {
      socket.end();
      process.exit(0);
    }, 500);
  }
}

socket.setNoDelay(true);
socket.on("connect", async () => {
  clearTimeout(connectTimer);
  console.log("tcp_connect_success");
  try {
    await writeLineAsync('{"v":1,"requestId":"hello-manual","type":"hello"}\n');
    helloTimer = setTimeout(() => {
      if (!sawHelloAck) {
        fail("hello_timeout");
      }
    }, helloTimeoutMs);
    helloTimer.unref();
  } catch (error) {
    fail(error instanceof Error && error.message === "write_timeout" ? "write_timeout" : "disconnected");
  }
});

socket.on("data", (chunk) => {
  buffer += chunk.toString("utf8").replace(/\r/g, "");
  while (true) {
    const newline = buffer.indexOf("\n");
    if (newline < 0) {
      return;
    }
    const line = buffer.slice(0, newline);
    buffer = buffer.slice(newline + 1);
    if (line.trim().length > 0) {
      handleLine(line);
    }
  }
});

socket.on("error", (error) => {
  if (!sawHelloAck) {
    fail(error.code === "ECONNREFUSED" ? "connect_timeout" : "disconnected", error.message);
  }
});

socket.on("close", () => {
  if (!sawHelloAck && process.exitCode !== 1) {
    fail("disconnected");
  }
});

socket.connect(port, host);
