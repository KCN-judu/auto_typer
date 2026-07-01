import assert from "node:assert/strict";
import { readFileSync } from "node:fs";

const keys = "1234567890-qwertyuiopasdfghjkl;'zxcvbnm,.- ".split("");
const keyPitchX = 19;
const rowOffsets = [0, 42.5, 25, 57.5, 137.5];
const rowY = [106, 87, 68, 49, 30];
const physicalRows = ["1234567890-=", "qwertyuiop[]", "asdfghjkl;'", "zxcvbnm,./"];
const keySet = new Set(keys);

function buildKeymap() {
  const bindings = [];
  physicalRows.forEach((rowKeys, row) => {
    for (let index = 0; index < rowKeys.length; index += 1) {
      const key = rowKeys[index];
      if (!keySet.has(key)) {
        continue;
      }
      bindings.push({ key, point: { xMm: rowOffsets[row] + index * keyPitchX, yMm: rowY[row] } });
    }
  });
  bindings.push({ key: " ", point: { xMm: rowOffsets[4], yMm: rowY[4] } });
  return bindings;
}

function lookupKey(key, keymap) {
  const normalized = key >= "A" && key <= "Z" ? key.toLowerCase() : key;
  return keymap.find((binding) => binding.key === normalized)?.point;
}

function planText(text, keymap, config) {
  const steps = [];
  let current = config.homePoint;
  for (let i = 0; i < text.length; i += 1) {
    const char = text[i];
    if (char === "\r" || char === "\n") {
      if (char === "\r" && text[i + 1] === "\n") {
        i += 1;
      }
      steps.push({ kind: "LineFeed", point: current, waitMs: 0 });
      current = { ...current, xMm: config.homePoint.xMm };
      continue;
    }
    const target = lookupKey(char, keymap);
    if (!target) {
      return { status: "KeyNotFound", failedKey: char, steps };
    }
    for (const step of [
      { kind: "MoveTo", point: target, waitMs: 0 },
      { kind: "Wait", point: target, waitMs: config.servo.settleMs },
      { kind: "Press", point: target, waitMs: config.servo.pressMs },
      { kind: "Release", point: target, waitMs: config.servo.releaseMs },
      { kind: "CharacterRelease", point: target, waitMs: 0 },
    ]) {
      if (steps.length >= 256) {
        return { status: "PlanFull", failedKey: "", steps };
      }
      steps.push(step);
    }
    current = target;
  }
  return { status: "Ok", failedKey: "", steps };
}

function stepsPerMm({ stepsPerRev, beltPitchMm, pulleyTeeth }) {
  return stepsPerRev / (beltPitchMm * pulleyTeeth);
}

function signedSteps(deltaMm, calibration) {
  const steps = Math.floor(Math.abs(deltaMm) * stepsPerMm(calibration) + 0.5);
  return deltaMm >= 0 ? steps : -steps;
}

function xyDeltaSteps(current, target, calibration) {
  const y = signedSteps(current.yMm - target.yMm, calibration);
  return {
    x: signedSteps(target.xMm - current.xMm, calibration),
    yLeft: y,
    yRight: -y,
    lineFeed: 0,
  };
}

function scaledRpm(axisSteps, primarySteps, maxRpm, minRpm) {
  if (axisSteps === 0 || primarySteps === 0) return 0;
  if (axisSteps >= primarySteps) return maxRpm;
  return Math.max(minRpm, Math.ceil((maxRpm * axisSteps) / primarySteps));
}

function parseEmmV5Frame({ canId, extd = true, rtr = false, data }) {
  const event = {
    kind: "None",
    motorId: 0,
    packetIndex: 0,
    command: data[0] ?? 0,
    status: 0,
    raw: data.slice(0, 8),
    dlc: Math.min(data.length, 8),
    canId,
    velocityRpm: 0,
    angleRaw65536: 0,
    inputPulseSteps: 0,
    statusFlags: 0,
    errorCode: "",
  };
  if (!extd) {
    return { ...event, kind: "InvalidFrame", errorCode: "not_extended_frame" };
  }
  if (rtr || event.dlc === 0) {
    return { ...event, kind: "InvalidFrame", errorCode: "non_data_frame" };
  }
  event.motorId = (canId >> 8) & 0xff;
  event.packetIndex = canId & 0xff;
  if (data[event.dlc - 1] !== 0x6b) {
    return { ...event, kind: "InvalidFrame", errorCode: "bad_checksum" };
  }
  if (event.dlc >= 3 && data[0] === 0x00 && data[1] === 0xee) {
    return { ...event, kind: "CommandMalformed", command: 0x00, status: 0xee };
  }
  if (event.dlc >= 3 && data[0] === 0xfd && data[1] === 0x9f) {
    return { ...event, kind: "MotionReached", command: 0xfd, status: 0x9f };
  }
  if (event.dlc >= 3 && data[1] === 0x02) {
    return { ...event, kind: "CommandAcked", command: data[0], status: 0x02 };
  }
  if (event.dlc >= 3 && data[1] === 0xe2) {
    return { ...event, kind: "CommandConditionNotMet", command: data[0], status: 0xe2 };
  }
  if (data[0] === 0x35 && event.dlc >= 5) {
    const rpm = (data[2] << 8) | data[3];
    return { ...event, kind: "VelocityFeedback", velocityRpm: data[1] === 0 ? rpm : -rpm };
  }
  if (data[0] === 0x36 && event.dlc >= 7) {
    const raw = ((data[2] << 24) >>> 0) | (data[3] << 16) | (data[4] << 8) | data[5];
    return { ...event, kind: "RealtimeAngleFeedback", angleRaw65536: data[1] === 0 ? raw : -raw };
  }
  if (data[0] === 0x32 && event.dlc >= 7) {
    const raw = ((data[2] << 24) >>> 0) | (data[3] << 16) | (data[4] << 8) | data[5];
    return { ...event, kind: "InputPulseFeedback", inputPulseSteps: data[1] === 0 ? raw : -raw };
  }
  if (data[0] === 0x3a || data[0] === 0x3b) {
    let flags = 0;
    for (const byte of data.slice(1, -1).slice(0, 4)) {
      flags = (flags << 8) | byte;
    }
    return { ...event, kind: data[0] === 0x3a ? "StatusFlagsFeedback" : "HomeStatusFeedback", statusFlags: flags };
  }
  return { ...event, kind: "UnknownFrame" };
}

const config = {
  homePoint: { xMm: 0, yMm: 0 },
  servo: { settleMs: 80, pressMs: 600, releaseMs: 300 },
  calibration: { beltPitchMm: 2, pulleyTeeth: 20, stepsPerRev: 3200 },
};

const keymap = buildKeymap();

assert.equal(keymap.length, keySet.size, "Feiyu 200 key count changed");
assert.deepEqual(lookupKey("1", keymap), { xMm: 0, yMm: 106 });
assert.deepEqual(lookupKey("q", keymap), { xMm: 42.5, yMm: 87 });
assert.deepEqual(lookupKey("a", keymap), { xMm: 25, yMm: 68 });
assert.deepEqual(lookupKey("z", keymap), { xMm: 57.5, yMm: 49 });
assert.deepEqual(lookupKey(" ", keymap), { xMm: 137.5, yMm: 30 });

const charPlan = planText("a", keymap, config);
assert.equal(charPlan.status, "Ok");
assert.deepEqual(charPlan.steps.map((step) => step.kind), ["MoveTo", "Wait", "Press", "Release", "CharacterRelease"]);

const newlinePlan = planText("\n", keymap, config);
assert.deepEqual(newlinePlan.steps.map((step) => step.kind), ["LineFeed"]);

assert.equal(planText("🙂", keymap, config).status, "KeyNotFound");
assert.equal(planText("a".repeat(60), keymap, config).status, "PlanFull");

assert.equal(stepsPerMm(config.calibration), 80);
assert.deepEqual(xyDeltaSteps({ xMm: 0, yMm: 0 }, { xMm: 10, yMm: 5 }, config.calibration), {
  x: 800,
  yLeft: -400,
  yRight: 400,
  lineFeed: 0,
});
assert.equal(scaledRpm(400, 800, 1600, 50), 800);
assert.equal(scaledRpm(1, 800, 1600, 50), 50);

assert.deepEqual(
  pick(parseEmmV5Frame({ canId: 0x0100, data: [0xfd, 0x02, 0x6b] }), ["motorId", "kind", "command"]),
  { motorId: 1, kind: "CommandAcked", command: 0xfd },
);
assert.deepEqual(
  pick(parseEmmV5Frame({ canId: 0x0200, data: [0xf3, 0xe2, 0x6b] }), ["motorId", "kind"]),
  { motorId: 2, kind: "CommandConditionNotMet" },
);
assert.equal(parseEmmV5Frame({ canId: 0x0300, data: [0x00, 0xee, 0x6b] }).kind, "CommandMalformed");
assert.equal(parseEmmV5Frame({ canId: 0x0100, data: [0xfd, 0x9f, 0x6b] }).kind, "MotionReached");
assert.equal(parseEmmV5Frame({ canId: 0x0100, data: [0x35, 0x01, 0x05, 0xdc, 0x6b] }).velocityRpm, -1500);
assert.equal(
  parseEmmV5Frame({ canId: 0x0100, data: [0x36, 0x01, 0x00, 0x01, 0x00, 0x00, 0x6b] }).angleRaw65536,
  -65536,
);
assert.equal(
  parseEmmV5Frame({ canId: 0x0100, data: [0x32, 0x01, 0x00, 0x00, 0x0c, 0x80, 0x6b] }).inputPulseSteps,
  -3200,
);
assert.deepEqual(
  pick(parseEmmV5Frame({ canId: 0x0100, extd: false, data: [0xfd, 0x02, 0x6b] }), ["kind", "errorCode", "motorId"]),
  { kind: "InvalidFrame", errorCode: "not_extended_frame", motorId: 0 },
);
assert.deepEqual(
  pick(parseEmmV5Frame({ canId: 0x0100, data: [0xfd, 0x02, 0x00] }), ["kind", "errorCode"]),
  { kind: "InvalidFrame", errorCode: "bad_checksum" },
);

const httpServer = readFileSync(new URL("../http_control_server.h", import.meta.url), "utf8");
assert.match(httpServer, /case JobState::None:[\s\S]*return "none";/, "JobState::None must serialize to none");
assert.match(httpServer, /request\["point"\]/, "ProbeKeyRequest must read nested point");
assert.match(httpServer, /sendJson\(200, response\);/, "Create job business rejections must return HTTP 200");
assert.match(httpServer, /rejectionMessage/, "CreateJobResponse must include rejection details");
assert.doesNotMatch(httpServer, /extractString|extractFloat|extractInt/, "HTTP handlers must not use ad-hoc JSON extractors");

const motionExecutor = readFileSync(new URL("../motion/MotionExecutor.h", import.meta.url), "utf8");
assert.doesNotMatch(motionExecutor, /bool feedbackSatisfied\(const MotionBlock&\)\s*\{\s*return false;\s*\}/, "feedbackSatisfied must use motor feedback");
assert.doesNotMatch(motionExecutor, /estimatedMoveMs/, "MotionExecutor must not complete moves from estimated duration");
assert.match(motionExecutor, /motion_feedback_timeout/, "Missing feedback must fault instead of completing");
assert.match(motionExecutor, /y_pair_skew/, "Y pair skew must fault");
assert.match(motionExecutor, /requestInputPulseCount/, "Motion feedback polling must request input pulse count");
assert.doesNotMatch(motionExecutor, /requestPosition\(/, "Motion feedback polling must not use realtime angle as position steps");
assert.match(motionExecutor, /motionPollIntervalMs/, "Motion feedback polling must be rate limited");
assert.match(motionExecutor, /lastFeedbackPollMs_/, "Motion feedback polling must keep a poll timestamp");
assert.match(motionExecutor, /inputPulseSteps/, "Motion completion must use input pulse steps");
assert.doesNotMatch(motionExecutor, /observedPositionSteps/, "Realtime angle must not be stored as observedPositionSteps");
assert.doesNotMatch(
  motionExecutor,
  /triggerSynchronousMotion\(config_\.topology\.(xMotorId|yLeftMotorId|yRightMotorId)\)/,
  "Coordinated X/Y motion must use broadcast sync trigger, not per-motor FF 66",
);

const emmV5Driver = readFileSync(new URL("../drivers/EmmV5Driver.h", import.meta.url), "utf8");
assert.match(emmV5Driver, /triggerSynchronousMotionBroadcast\(\)/, "EMM_V5 driver must expose broadcast sync trigger");
assert.match(
  emmV5Driver,
  /\{\s*0x00,\s*0xFF,\s*0x66,\s*0x6B\s*\}/,
  "Broadcast sync trigger must send address 0 with FF 66 6B",
);

const yPairController = readFileSync(new URL("../motion/YPairController.h", import.meta.url), "utf8");
assert.match(yPairController, /triggerSynchronousMotionBroadcast\(\)/, "YPair move must use broadcast sync trigger");
assert.doesNotMatch(
  yPairController,
  /triggerSynchronousMotion\(config_\.topology\.y(Left|Right)MotorId\)/,
  "YPair move must not send per-motor FF 66 triggers",
);

const autoTyperRuntime = readFileSync(new URL("../auto_typer_runtime.h", import.meta.url), "utf8");
assert.doesNotMatch(
  autoTyperRuntime,
  /yPair\.moveRelative\([\s\S]*?&&\s*yPair\.trigger\(\)/,
  "YPair debug move must not add a second sync trigger",
);

const canTxQueue = readFileSync(new URL("../can/CanTxQueue.h", import.meta.url), "utf8");
assert.match(canTxQueue, /pendingValid_/, "CAN TX queue must keep a pending frame after transmit failure");
assert.match(canTxQueue, /pendingFrame_/, "CAN TX queue must store the frame being retried");
assert.match(canTxQueue, /recordTxRetry\(\)/, "CAN TX retry must increment diagnostics");
assert.match(canTxQueue, /addTxRetry/, "CAN TX retry must be visible in protocol trace");
assert.match(
  canTxQueue,
  /if\s*\(\s*!bus_\.transmit\(pendingFrame_\.frame\)\s*\)[\s\S]*?return;/,
  "CAN TX failure must retain pending frame for the next tick",
);
assert.match(
  canTxQueue,
  /pendingValid_\s*=\s*false;[\s\S]*?setPendingFrameValid\(false\);[\s\S]*?\+\+sent;/,
  "CAN TX pending frame must clear only after transmit success",
);
assert.match(
  canTxQueue,
  /hasFatalFault\(\)[\s\S]*?!pendingFrame_\.highPriority[\s\S]*?pendingValid_\s*=\s*false/,
  "Fatal CAN fault must drop ordinary pending frames",
);
assert.match(
  canTxQueue,
  /hasFatalFault\(\)[\s\S]*?!item\.highPriority[\s\S]*?continue;/,
  "Fatal CAN fault must skip ordinary queued frames so stop frames can pass",
);

function simulateCanTxQueue({ frames, failAttempts = new Set(), fatalFault = false }) {
  const queue = [...frames];
  const sent = [];
  const retries = [];
  const dropped = [];
  let pendingValid = false;
  let pendingFrame = undefined;
  let attempt = 0;

  function loadNextPendingFrame() {
    while (queue.length > 0) {
      const item = queue.shift();
      if (fatalFault && !item.highPriority) {
        dropped.push(item.frame.id);
        continue;
      }
      pendingFrame = item;
      pendingValid = true;
      return true;
    }
    return false;
  }

  function tick(maxFrames = 4) {
    let sentThisTick = 0;
    while (sentThisTick < maxFrames) {
      if (fatalFault && pendingValid && !pendingFrame.highPriority) {
        dropped.push(pendingFrame.frame.id);
        pendingValid = false;
      }
      if (!pendingValid && !loadNextPendingFrame()) {
        return;
      }
      attempt += 1;
      if (failAttempts.has(attempt)) {
        retries.push(pendingFrame.frame.id);
        return;
      }
      sent.push(pendingFrame.frame.id);
      pendingValid = false;
      sentThisTick += 1;
    }
  }

  return {
    tick,
    sent,
    retries,
    dropped,
    pendingId: () => (pendingValid ? pendingFrame.frame.id : undefined),
  };
}

const retryQueue = simulateCanTxQueue({
  frames: [{ frame: { id: "0x0100", data: [0xfd, 0x01] }, highPriority: false }],
  failAttempts: new Set([1]),
});
retryQueue.tick();
assert.equal(retryQueue.pendingId(), "0x0100", "Failed CAN TX must remain pending");
assert.deepEqual(retryQueue.retries, ["0x0100"], "Failed CAN TX must be counted as retry");
retryQueue.tick();
assert.deepEqual(retryQueue.sent, ["0x0100"], "Next tick must resend the same CAN frame");
assert.equal(retryQueue.pendingId(), undefined, "Successful retry must clear pending frame");

const multiFrameQueue = simulateCanTxQueue({
  frames: [
    { frame: { id: "0x0100", data: [0xfd, 0x01] }, highPriority: false },
    { frame: { id: "0x0101", data: [0xfd, 0x02] }, highPriority: false },
    { frame: { id: "0x0102", data: [0xfd, 0x03] }, highPriority: false },
  ],
  failAttempts: new Set([2]),
});
multiFrameQueue.tick();
assert.deepEqual(multiFrameQueue.sent, ["0x0100"], "Frames before a TX failure should remain sent in order");
assert.equal(multiFrameQueue.pendingId(), "0x0101", "Middle FD packet must remain pending after TX failure");
multiFrameQueue.tick();
assert.deepEqual(
  multiFrameQueue.sent,
  ["0x0100", "0x0101", "0x0102"],
  "Multi-frame FD commands must not lose or reorder packets after a TX failure",
);

const fatalQueue = simulateCanTxQueue({
  frames: [
    { frame: { id: "normal" }, highPriority: false },
    { frame: { id: "stop" }, highPriority: true },
  ],
  fatalFault: true,
});
fatalQueue.tick();
assert.deepEqual(fatalQueue.dropped, ["normal"], "Fatal CAN fault may drop ordinary queued frames");
assert.deepEqual(fatalQueue.sent, ["stop"], "Fatal CAN fault must still attempt high-priority stop frames");

const protocolTypes = readFileSync(new URL("../protocol/EmmV5ProtocolParser.h", import.meta.url), "utf8");
assert.match(protocolTypes, /InputPulseFeedback/, "Parser event model must include input pulse feedback");
assert.match(protocolTypes, /RealtimeAngleFeedback/, "Parser event model must keep realtime angle separate");

function pick(object, keys) {
  return Object.fromEntries(keys.map((key) => [key, object[key]]));
}

console.log("firmware planner/kinematics regression tests passed");
