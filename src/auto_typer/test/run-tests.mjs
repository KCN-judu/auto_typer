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
      { kind: "Wait", point: target, waitMs: config.pressMotor.settleMs },
      { kind: "Press", point: target, waitMs: config.pressMotor.pressMs },
      { kind: "Release", point: target, waitMs: config.pressMotor.releaseMs },
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

function signedStepsForDirection(steps, direction) {
  return direction === "cw" ? steps : -steps;
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
  if (data[0] === 0x32 && event.dlc >= 7) {
    const raw = ((data[2] << 24) >>> 0) | (data[3] << 16) | (data[4] << 8) | data[5];
    return { ...event, kind: "InputPulseFeedback", inputPulseSteps: data[1] === 0 ? raw : -raw };
  }
  if (data[0] === 0x35 && event.dlc >= 5) {
    const rpm = (data[2] << 8) | data[3];
    return { ...event, kind: "VelocityFeedback", velocityRpm: data[1] === 0 ? rpm : -rpm };
  }
  if (data[0] === 0x36 && event.dlc >= 7) {
    const raw = ((data[2] << 24) >>> 0) | (data[3] << 16) | (data[4] << 8) | data[5];
    return { ...event, kind: "RealtimeAngleFeedback", angleRaw65536: data[1] === 0 ? raw : -raw };
  }
  if (data[0] === 0x3a || data[0] === 0x3b) {
    let flags = 0;
    for (const byte of data.slice(1, -1).slice(0, 4)) {
      flags = (flags << 8) | byte;
    }
    return { ...event, kind: data[0] === 0x3a ? "StatusFlagsFeedback" : "HomeStatusFeedback", statusFlags: flags };
  }
  if (event.dlc === 3 && data[0] === 0x00 && data[1] === 0xee) {
    return { ...event, kind: "CommandMalformed", command: 0x00, status: 0xee };
  }
  if (event.dlc === 3 && data[0] === 0xfd && data[1] === 0x9f) {
    return { ...event, kind: "MotionReached", command: 0xfd, status: 0x9f };
  }
  if (event.dlc === 3 && data[0] === 0xfd && data[1] === 0x02) {
    return { ...event, kind: "CommandAcked", command: 0xfd, status: 0x02 };
  }
  if (event.dlc === 3 && data[0] === 0xfd && data[1] === 0xe2) {
    return { ...event, kind: "CommandConditionNotMet", command: 0xfd, status: 0xe2 };
  }
  return { ...event, kind: "UnknownFrame" };
}

function planCharacterReleaseDelta(direction, steps) {
  return signedStepsForDirection(steps, direction);
}

function planLineFeedDelta(direction, steps) {
  return signedStepsForDirection(steps, direction);
}

function lineFeedTarget(initialPulse, signedDelta) {
  return initialPulse + signedDelta;
}

function canMotionReady({ ready, fatalFault }) {
  return ready && !fatalFault;
}

function applyFeedbackEvent(state, event) {
  const next = { ...state, id: event.motorId };
  switch (event.kind) {
    case "CommandAcked":
      next.lastAckCommand = event.command;
      next.lastAckMs = event.timeMs;
      break;
    case "CommandConditionNotMet":
      next.conditionNotMet = true;
      next.lastConditionNotMetCommand = event.command;
      next.lastConditionNotMetMs = event.timeMs;
      next.lastErrorCode = "condition_not_met";
      break;
    case "CommandMalformed":
      next.commandMalformed = true;
      next.lastMalformedCommand = event.command;
      next.lastMalformedMs = event.timeMs;
      next.lastErrorCode = "command_malformed";
      break;
    case "MotionReached":
      next.motionReached = true;
      next.lastMotionReachedMs = event.timeMs;
      break;
    case "InputPulseFeedback":
      next.hasInputPulse = true;
      next.inputPulseSteps = event.inputPulseSteps;
      next.lastInputPulseMs = event.timeMs;
      break;
    case "VelocityFeedback":
      next.hasVelocity = true;
      next.velocityRpm = event.velocityRpm;
      next.lastVelocityMs = event.timeMs;
      break;
    case "RealtimeAngleFeedback":
      next.hasRealtimeAngle = true;
      next.realtimeAngleRaw65536 = event.angleRaw65536;
      next.lastRealtimeAngleMs = event.timeMs;
      break;
    case "StatusFlagsFeedback":
    case "HomeStatusFeedback":
      next.hasStatus = true;
      next.statusFlags = event.statusFlags;
      next.lastStatusMs = event.timeMs;
      break;
  }
  return next;
}

function emptyMotorState(id) {
  return {
    id,
    hasInputPulse: false,
    hasVelocity: false,
    hasRealtimeAngle: false,
    hasStatus: false,
    inputPulseSteps: 0,
    velocityRpm: 0,
    realtimeAngleRaw65536: 0,
    statusFlags: 0,
    lastInputPulseMs: 0,
    lastVelocityMs: 0,
    lastRealtimeAngleMs: 0,
    lastStatusMs: 0,
  };
}

function encodeCommandFrames(command) {
  const payloadLen = command.length - 2;
  const frames = [];
  let offset = 0;
  let packetIndex = 0;
  while (offset < payloadLen) {
    const remaining = payloadLen - offset;
    const chunkLen = Math.min(remaining, 7);
    const data = [command[1]];
    for (let i = 0; i < chunkLen; i += 1) {
      data.push(command[offset + 2]);
      offset += 1;
    }
    frames.push({
      canId: (command[0] << 8) | packetIndex,
      extd: true,
      data,
    });
    packetIndex += 1;
  }
  return frames;
}

function moveCommand({ motorId, direction, rpm, acceleration, steps, sync }) {
  return [
    motorId,
    0xfd,
    direction === "ccw" ? 1 : 0,
    (rpm >> 8) & 0xff,
    rpm & 0xff,
    acceleration,
    (steps >> 24) & 0xff,
    (steps >> 16) & 0xff,
    (steps >> 8) & 0xff,
    steps & 0xff,
    0x00,
    sync ? 1 : 0,
    0x6b,
  ];
}

function moveRelativeBatch(commands, broadcastTrigger) {
  const frames = commands.flatMap((command) => encodeCommandFrames(moveCommand(command)));
  if (broadcastTrigger) {
    frames.push(...encodeCommandFrames([0x00, 0xff, 0x66, 0x6b]));
  }
  return frames;
}

function yOnlyFrames(yLeftSteps) {
  const direction = yLeftSteps >= 0 ? "cw" : "ccw";
  const mirrored = direction === "cw" ? "ccw" : "cw";
  const steps = Math.abs(yLeftSteps);
  return moveRelativeBatch(
    [
      { motorId: 2, direction, rpm: 1600, acceleration: 128, steps, sync: true },
      { motorId: 3, direction: mirrored, rpm: 1600, acceleration: 128, steps, sync: true },
    ],
    true,
  );
}

function xyFrames({ xSteps, yLeftSteps }) {
  const yDirection = yLeftSteps >= 0 ? "cw" : "ccw";
  const yMirrored = yDirection === "cw" ? "ccw" : "cw";
  return moveRelativeBatch(
    [
      { motorId: 1, direction: xSteps >= 0 ? "cw" : "ccw", rpm: 1600, acceleration: 128, steps: Math.abs(xSteps), sync: true },
      { motorId: 2, direction: yDirection, rpm: 1600, acceleration: 128, steps: Math.abs(yLeftSteps), sync: true },
      { motorId: 3, direction: yMirrored, rpm: 1600, acceleration: 128, steps: Math.abs(yLeftSteps), sync: true },
    ],
    true,
  );
}

function countBroadcastTriggers(frames) {
  return frames.filter((frame) => frame.canId === 0x0000 && frame.data.join(",") === "255,102,107").length;
}

function countMotorSpecificTriggers(frames) {
  return frames.filter((frame) => frame.canId !== 0x0000 && frame.data.join(",") === "255,102,107").length;
}

function baselineReady(states, requiredIds, nowMs, maxAgeMs) {
  return requiredIds.every((id) => {
    const state = states.get(id);
    return (
      state?.hasInputPulse &&
      state.hasVelocity &&
      state.lastInputPulseMs !== 0 &&
      state.lastVelocityMs !== 0 &&
      nowMs - state.lastInputPulseMs <= maxAgeMs &&
      nowMs - state.lastVelocityMs <= maxAgeMs
    );
  });
}

function motorAtTarget(state, target, nowMs, { maxAgeMs, toleranceSteps, stopVelocityRpm }) {
  if (!state.hasInputPulse || !state.hasVelocity) return false;
  if (nowMs - state.lastInputPulseMs > maxAgeMs || nowMs - state.lastVelocityMs > maxAgeMs) return false;
  return Math.abs(state.inputPulseSteps - target) <= toleranceSteps && Math.abs(state.velocityRpm) <= stopVelocityRpm;
}

function yPairSkewExceeded({ leftStart, rightStart, leftNow, rightNow, tolerance }) {
  const leftDelta = leftNow - leftStart;
  const rightDelta = rightNow - rightStart;
  return Math.abs(leftDelta + rightDelta) > tolerance;
}

const config = {
  homePoint: { xMm: 0, yMm: 0 },
  pressMotor: { settleMs: 80, pressMs: 600, releaseMs: 300 },
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
assert.equal(planCharacterReleaseDelta("cw", 180), 180);
assert.equal(planCharacterReleaseDelta("ccw", 180), -180);
assert.equal(planLineFeedDelta("cw", 16440), 16440);
assert.equal(planLineFeedDelta("ccw", 16440), -16440);
assert.equal(lineFeedTarget(5000, -180), 4820);
assert.equal(lineFeedTarget(-200, 16440), 16240);

assert.deepEqual(
  pick(parseEmmV5Frame({ canId: 0x0100, data: [0xfd, 0x02, 0x6b] }), ["motorId", "kind", "command"]),
  { motorId: 1, kind: "CommandAcked", command: 0xfd },
);
assert.deepEqual(
  pick(parseEmmV5Frame({ canId: 0x0200, data: [0xf3, 0xe2, 0x6b] }), ["motorId", "kind"]),
  { motorId: 2, kind: "UnknownFrame" },
);
assert.deepEqual(
  pick(parseEmmV5Frame({ canId: 0x0200, data: [0xfd, 0xe2, 0x6b] }), ["motorId", "kind", "command"]),
  { motorId: 2, kind: "CommandConditionNotMet", command: 0xfd },
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
assert.equal(
  parseEmmV5Frame({ canId: 0x0100, data: [0x35, 0x02, 0x00, 0x10, 0x00, 0x6b] }).kind,
  "VelocityFeedback",
);
assert.equal(
  parseEmmV5Frame({ canId: 0x0100, data: [0x32, 0x02, 0x00, 0x00, 0x01, 0x00, 0x6b] }).kind,
  "InputPulseFeedback",
);
assert.deepEqual(
  pick(parseEmmV5Frame({ canId: 0x0100, extd: false, data: [0xfd, 0x02, 0x6b] }), ["kind", "errorCode", "motorId"]),
  { kind: "InvalidFrame", errorCode: "not_extended_frame", motorId: 0 },
);
assert.deepEqual(
  pick(parseEmmV5Frame({ canId: 0x0100, data: [0xfd, 0x02, 0x00] }), ["kind", "errorCode"]),
  { kind: "InvalidFrame", errorCode: "bad_checksum" },
);
assert.deepEqual(
  pick(parseEmmV5Frame({ canId: 0x0107, data: [0x77, 0x01, 0x6b] }), ["motorId", "packetIndex", "kind"]),
  { motorId: 1, packetIndex: 7, kind: "UnknownFrame" },
);
assert.equal(canMotionReady({ ready: true, fatalFault: false }), true);
assert.equal(canMotionReady({ ready: true, fatalFault: true }), false);
assert.equal(canMotionReady({ ready: false, fatalFault: false }), false);

const pulseEvent = { ...parseEmmV5Frame({ canId: 0x0100, data: [0x32, 0x00, 0x00, 0x00, 0x03, 0xe8, 0x6b] }), timeMs: 100 };
const velocityEvent = { ...parseEmmV5Frame({ canId: 0x0100, data: [0x35, 0x00, 0x00, 0x00, 0x6b] }), timeMs: 120 };
const angleEvent = { ...parseEmmV5Frame({ canId: 0x0100, data: [0x36, 0x00, 0x00, 0x01, 0x00, 0x00, 0x6b] }), timeMs: 140 };
let feedbackState = emptyMotorState(1);
feedbackState = applyFeedbackEvent(feedbackState, pulseEvent);
assert.equal(feedbackState.inputPulseSteps, 1000, "0x32 input pulse must update pulse steps");
feedbackState = applyFeedbackEvent(feedbackState, angleEvent);
assert.equal(feedbackState.inputPulseSteps, 1000, "0x36 realtime angle must not overwrite pulse steps");
assert.equal(feedbackState.realtimeAngleRaw65536, 65536, "0x36 realtime angle must update angle only");
feedbackState = applyFeedbackEvent(feedbackState, velocityEvent);
assert.equal(
  motorAtTarget(feedbackState, 1000, 150, { maxAgeMs: 300, toleranceSteps: 16, stopVelocityRpm: 1 }),
  true,
  "Completion model requires fresh pulse and velocity near target",
);
assert.equal(
  motorAtTarget({ ...feedbackState, velocityRpm: 20, lastVelocityMs: 150 }, 1000, 160, {
    maxAgeMs: 300,
    toleranceSteps: 16,
    stopVelocityRpm: 1,
  }),
  false,
  "Motion completion must reject nonzero velocity",
);

const baselineStates = new Map([[1, feedbackState]]);
assert.equal(
  baselineReady(new Map([[1, { ...feedbackState, hasVelocity: false, lastVelocityMs: 0 }]]), [1], 150, 1500),
  false,
  "Fresh 0x32 pulse without 0x35 velocity must not satisfy baseline",
);
assert.equal(
  baselineReady(baselineStates, [1], 150, 1500),
  true,
  "Fresh 0x32 pulse and 0x35 velocity should satisfy baseline",
);
assert.equal(baselineReady(baselineStates, [1], 2000, 1500), false, "Stale feedback should not satisfy baseline");

const yOnly = yOnlyFrames(-320);
assert.equal(countBroadcastTriggers(yOnly), 1, "Y-only movement must emit exactly one broadcast trigger");
assert.equal(countMotorSpecificTriggers(yOnly), 0, "Y-only movement must not emit motor-specific triggers");
assert.deepEqual(
  yOnly.filter((frame) => frame.data[0] === 0xfd).map((frame) => frame.canId),
  [0x0200, 0x0201, 0x0300, 0x0301],
  "Y-only FD frames must target motors 2 and 3 with split packet indices",
);
assert.equal(yOnly.filter((frame) => frame.data[0] === 0xfd).every((frame) => frame.data[0] === 0xfd), true);

const xyLinked = xyFrames({ xSteps: 400, yLeftSteps: -320 });
assert.equal(countBroadcastTriggers(xyLinked), 1, "X+Y movement must emit exactly one broadcast trigger");
assert.equal(countMotorSpecificTriggers(xyLinked), 0, "X+Y movement must not emit motor-specific triggers");
assert.deepEqual(
  xyLinked.filter((frame) => frame.data[0] === 0xfd).map((frame) => frame.canId),
  [0x0100, 0x0101, 0x0200, 0x0201, 0x0300, 0x0301],
  "X+Y FD frames must target X, Y-left, and Y-right with split packet indices",
);

assert.equal(yPairSkewExceeded({ leftStart: 100, rightStart: -100, leftNow: -220, rightNow: 220, tolerance: 10 }), false);
assert.equal(yPairSkewExceeded({ leftStart: 100, rightStart: -100, leftNow: -220, rightNow: 260, tolerance: 10 }), true);

const httpServer = readFileSync(new URL("../http_control_server.h", import.meta.url), "utf8");
assert.match(httpServer, /case JobState::None:[\s\S]*return "none";/, "JobState::None must serialize to none");
assert.match(httpServer, /request\["point"\]/, "ProbeKeyRequest must read nested point");
assert.match(httpServer, /sendJson\(200, response\);/, "Create job business rejections must return HTTP 200");
assert.match(httpServer, /rejectionMessage/, "CreateJobResponse must include rejection details");
assert.match(httpServer, /diagnostics\/protocol-trace/, "Protocol trace diagnostics route must be registered");
assert.match(httpServer, /machine\/probe-motors/, "Motor probe route must be registered");
assert.match(httpServer, /\/api\/wifi\/status/, "WiFi setup status route must be registered");
assert.match(httpServer, /\/api\/wifi\/networks/, "WiFi network scan route must be registered");
assert.match(httpServer, /\/api\/wifi\/config/, "WiFi config route must be registered");
assert.match(httpServer, /\/api\/wifi\/setup\/finish/, "WiFi setup finish route must be registered");
assert.match(httpServer, /WIFI_AP_STA/, "WiFi setup must use AP+STA mode");
assert.match(httpServer, /Preferences prefs;[\s\S]*wifiSsid[\s\S]*wifiPass/, "WiFi credentials must be saved in Preferences");
assert.match(httpServer, /WiFi\.scanNetworks/, "WiFi setup must scan networks from the ESP32");
assert.doesNotMatch(
  httpServer,
  /json\["staPassword"\]|json\["wifiPassword"\]|response\["staPassword"\]|response\["wifiPassword"\]/,
  "WiFi status must not return station WiFi password",
);
assert.match(httpServer, /not_ready/, "HTTP status must expose not_ready health");
assert.match(httpServer, /motorReadinessJson/, "Motor status must serialize readiness");
assert.match(httpServer, /motorRoleJson/, "Motor status must serialize motor roles");
assert.match(httpServer, /pressMotorId/, "HTTP status must include the press motor");
assert.match(httpServer, /MotorRole::Press[\s\S]*return "press";/, "HTTP motor roles must serialize the press motor");
assert.match(httpServer, /item\["dataHex"\]/, "Protocol trace API must expose hex data");
assert.match(httpServer, /writeProtocolDiagnostics/, "Protocol trace API must expose parser diagnostics");
assert.match(httpServer, /commandQueueFullCount/, "CAN diagnostics API must expose command queue full count");
assert.match(httpServer, /lastCommandQueueError/, "CAN diagnostics API must expose last command queue error");
assert.doesNotMatch(httpServer, /extractString|extractFloat|extractInt/, "HTTP handlers must not use ad-hoc JSON extractors");

const autoTyperRuntime = readFileSync(new URL("../auto_typer_runtime.h", import.meta.url), "utf8");
assert.match(autoTyperRuntime, /prepareMotorsBestEffort/, "Startup motor preparation must be best effort");
assert.match(
  autoTyperRuntime,
  /void prepareMotorsBestEffort\(\)\s*\{\s*probeMotorsBestEffort\(150\);\s*\}/,
  "Startup motor preparation must only run the best-effort probe",
);
assert.doesNotMatch(autoTyperRuntime, /deviceReadyWarning_/, "Readiness warning must not be sticky state");
assert.match(autoTyperRuntime, /checkRequiredActuators/, "Job acceptance must explicitly check required actuators");
assert.match(autoTyperRuntime, /x_motor_not_ready|y_pair_not_ready|line_feed_not_ready|press_motor_not_ready/, "Required actuator failure must reject the job");
assert.match(autoTyperRuntime, /hasInputPulse/, "Required motor readiness must require pulse feedback");
assert.match(autoTyperRuntime, /lastConditionNotMetMs/, "Startup E2 must leave the device not ready for jobs");
assert.match(autoTyperRuntime, /lastMalformedMs/, "Startup EE must leave the device not ready for jobs");
assert.match(autoTyperRuntime, /kRecentAlertWindowMs/, "Health warning must use a recent alert window");
assert.match(autoTyperRuntime, /config_\.topology\.pressMotorId/, "Runtime must track and probe the press motor");
assert.match(autoTyperRuntime, /kTrackedMotorCount\s*=\s*5/, "Runtime must reserve probe state for five motors");

const protocol = readFileSync(new URL("../../../shared/protocol/auto-typer-protocol.ts", import.meta.url), "utf8");
assert.match(protocol, /not_ready/, "Shared protocol must include not_ready health");
assert.match(protocol, /WifiSetupStatusResponse/, "Shared protocol must expose WiFi setup status");
assert.match(protocol, /wifiNetworks:\s*"\/api\/wifi\/networks"/, "Shared protocol must include WiFi scan route");
assert.match(protocol, /wifiConfig:\s*"\/api\/wifi\/config"/, "Shared protocol must include WiFi config route");
assert.match(protocol, /MotorReadiness/, "Shared protocol must include motor readiness");
assert.match(protocol, /"press"/, "Shared protocol must include the press motor role");
assert.match(protocol, /probeMotors/, "Shared protocol must include probe motors route");
assert.match(protocol, /tx_queued[\s\S]*tx_sent[\s\S]*tx_retry[\s\S]*rx/, "Shared protocol must type protocol trace directions");

const motionExecutor = readFileSync(new URL("../motion/MotionExecutor.h", import.meta.url), "utf8");
assert.doesNotMatch(motionExecutor, /bool feedbackSatisfied\(const MotionStep&\)\s*\{\s*return false;\s*\}/, "feedbackSatisfied must use motor feedback");
assert.doesNotMatch(motionExecutor, /estimatedMoveMs/, "MotionExecutor must not complete moves from estimated duration");
assert.match(motionExecutor, /motor_feedback_baseline_timeout/, "Missing baseline must fault before sending motion commands");
assert.match(motionExecutor, /motion_feedback_timeout/, "Stale feedback after command send must fault");
assert.match(motionExecutor, /motion_timeout/, "Overall motion timeout must use a distinct fault code");
assert.match(motionExecutor, /y_pair_skew/, "Y pair skew must fault");
assert.match(motionExecutor, /requestInputPulseCount/, "Motion feedback polling must request input pulse count");
assert.doesNotMatch(motionExecutor, /requestPosition\(/, "Motion feedback polling must not use realtime angle as position steps");
assert.match(motionExecutor, /motionPollIntervalMs/, "Motion feedback polling must be rate limited");
assert.match(motionExecutor, /lastFeedbackPollMs_/, "Motion feedback polling must keep a poll timestamp");
assert.match(motionExecutor, /inputPulseSteps/, "Motion completion must use input pulse steps");
assert.match(motionExecutor, /validateLineFeedBaseline/, "Line feed moves must require a fresh feedback baseline");
assert.match(motionExecutor, /line_feed_baseline_missing/, "Missing line feed baseline must fail with a clear error");
assert.match(motionExecutor, /prepareStepBaseline/, "MotionExecutor must acquire a fresh baseline before command send");
assert.match(motionExecutor, /baselineReady\(step\)/, "MotionExecutor must gate command send on baseline readiness");
assert.match(motionExecutor, /captureFeedbackTargets\(step\)[\s\S]*return driver_\.moveRelative/, "Line feed targets must be captured after baseline before FD send");
assert.doesNotMatch(motionExecutor, /observedPositionSteps/, "Realtime angle must not be stored as observedPositionSteps");
assert.doesNotMatch(
  motionExecutor,
  /triggerSynchronousMotion\(config_\.topology\.(xMotorId|yLeftMotorId|yRightMotorId)\)/,
  "Coordinated X/Y motion must use broadcast sync trigger, not per-motor FF 66",
);
assert.match(
  motionExecutor,
  /moveRelativeBatch\(commands,\s*sizeof\(commands\) \/ sizeof\(commands\[0\]\),\s*true\)/,
  "Coordinated X/Y motion must atomically enqueue X, Y pair, and broadcast trigger",
);

const emmV5Driver = readFileSync(new URL("../drivers/EmmV5Driver.h", import.meta.url), "utf8");
assert.match(emmV5Driver, /triggerSynchronousMotionBroadcast\(\)/, "EMM_V5 driver must expose broadcast sync trigger");
assert.match(emmV5Driver, /requestRealtimeAngle/, "EMM_V5 realtime-angle request must be named explicitly");
assert.doesNotMatch(emmV5Driver, /requestPosition\(/, "EMM_V5 realtime angle request must not be exposed as requestPosition");
assert.match(
  emmV5Driver,
  /\{\s*0x00,\s*0xFF,\s*0x66,\s*0x6B\s*\}/,
  "Broadcast sync trigger must send address 0 with FF 66 6B",
);
assert.match(emmV5Driver, /MoveRelativeCommand/, "EMM_V5 driver must expose a batchable move descriptor");
assert.match(emmV5Driver, /moveRelativeBatch/, "EMM_V5 driver must support atomic grouped moves");
assert.match(emmV5Driver, /appendCommandFrames/, "EMM_V5 commands must be fully encoded before enqueue");
assert.match(
  emmV5Driver,
  /enqueueBatch\(frames,\s*frameCount/,
  "EMM_V5 driver must enqueue encoded command frames as a batch",
);
assert.doesNotMatch(
  emmV5Driver,
  /while\s*\(\s*offset\s*<\s*payloadLen\s*\)[\s\S]*?tx_\.enqueue\(/,
  "EMM_V5 driver must not enqueue frames while encoding a command",
);

const yPairController = readFileSync(new URL("../motion/YPairController.h", import.meta.url), "utf8");
assert.match(yPairController, /triggerSynchronousMotionBroadcast\(\)/, "YPair move must use broadcast sync trigger");
assert.match(yPairController, /moveRelativeBatch/, "YPair move must submit motor 2, motor 3, and trigger atomically");
assert.doesNotMatch(
  yPairController,
  /triggerSynchronousMotion\(config_\.topology\.y(Left|Right)MotorId\)/,
  "YPair move must not send per-motor FF 66 triggers",
);
assert.doesNotMatch(
  yPairController,
  /driver_\.moveRelative\([\s\S]*?&&[\s\S]*?driver_\.moveRelative\(/,
  "YPair move must not enqueue left and right FD commands independently",
);

assert.doesNotMatch(
  autoTyperRuntime,
  /triggerSynchronousMotionBroadcast\(\)/,
  "Single-motor debug moves must not issue an extra broadcast sync trigger",
);

assert.match(
  motionExecutor,
  /stopNow\(config_\.topology\.pressMotorId\)/,
  "Emergency stop must also stop the press motor",
);

const canTxQueue = readFileSync(new URL("../can/CanTxQueue.h", import.meta.url), "utf8");
assert.match(canTxQueue, /availableForWrite\(\)/, "CAN TX queue must expose available write capacity");
assert.match(canTxQueue, /enqueueBatch/, "CAN TX queue must support atomic batch enqueue");
assert.match(canTxQueue, /uxQueueSpacesAvailable\(queue_\)\s*<\s*count/, "CAN TX batch enqueue must preflight capacity");
assert.match(canTxQueue, /recordCommandQueueFull\(\)/, "CAN TX batch enqueue must record command_queue_full");
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
assert.match(protocolTypes, /event\.command == 0x32/, "Parser must prioritize 0x32 input pulse telemetry");
assert.match(protocolTypes, /event\.dlc == 3 && event\.raw\[0\] == 0xFD && event\.raw\[1\] == 0x02/, "Parser ACK detection must be constrained to short FD ACK frames");

const protocolTrace = readFileSync(new URL("../can/ProtocolTrace.h", import.meta.url), "utf8");
assert.match(protocolTrace, /kCapacity\s*=\s*128/, "Protocol trace must retain at least 128 frames");
assert.match(protocolTrace, /dataHex\[24\]/, "Protocol trace items must include fixed hex data storage");
assert.match(protocolTrace, /writeDataHex/, "Protocol trace must format raw data as hex");

const eventStore = readFileSync(new URL("../can/EmmV5EventStore.h", import.meta.url), "utf8");
assert.match(eventStore, /UnknownFrame[\s\S]*\+\+unknownFrameCount_/, "Unknown frames must be retained in diagnostics");
assert.match(eventStore, /InvalidFrame[\s\S]*\+\+invalidFrameCount_/, "Invalid frames must be retained in diagnostics");

const canBus = readFileSync(new URL("../can/CanBus.h", import.meta.url), "utf8");
assert.doesNotMatch(canBus, /registerSoftFault/, "Soft CAN faults must not escalate into fatal faults");
assert.match(canBus, /TWAI_ALERT_BUS_OFF/, "CAN bus-off must still be handled as fatal");
assert.match(canBus, /const esp_err_t startResult = twai_start\(\);/, "CAN bus recovery must not call twai_start twice");

const machineKinematics = readFileSync(new URL("../motion/MachineKinematics.h", import.meta.url), "utf8");
assert.match(machineKinematics, /signedStepsForDirection/, "Machine kinematics must expose signed direction helper");

const motionPlanner = readFileSync(new URL("../motion/MotionPlanner.h", import.meta.url), "utf8");
assert.match(motionPlanner, /signedStepsForDirection\(config\.lineFeed\.characterReleaseSteps, config\.lineFeed\.releaseDirection\)/, "Character release must plan signed line-feed delta");
assert.match(motionPlanner, /signedStepsForDirection\(config\.lineFeed\.returnTotalSteps, config\.lineFeed\.returnDirection\)/, "Line feed must plan signed return delta");

const sharedProtocol = readFileSync(new URL("../../../shared/protocol/auto-typer-protocol.ts", import.meta.url), "utf8");
assert.match(sharedProtocol, /dataHex: string/, "Shared protocol must type protocol trace hex data");
assert.match(sharedProtocol, /unknownFrameCount: number/, "Shared protocol must type parser diagnostics");
assert.match(sharedProtocol, /commandQueueFullCount: number/, "Shared protocol must type command queue full diagnostics");
assert.match(sharedProtocol, /lastCommandQueueError: string/, "Shared protocol must type last command queue error");
assert.match(sharedProtocol, /export type GroupStreamMessage/, "Shared protocol must define group stream messages");
assert.match(sharedProtocol, /export type RemoteMotionStep/, "Shared protocol must define remote motion steps");
assert.doesNotMatch(sharedProtocol, /type: "exec_block"|type: "block_started"|type: "block_done"|RemoteMotionBlock|BlockStream/, "Shared group stream protocol must not expose legacy block messages");
assert.match(sharedProtocol, /export type RemoteMotionGroup/, "Shared protocol must define remote motion groups");
assert.match(sharedProtocol, /type: "exec_group"[\s\S]*groupId: string[\s\S]*steps: RemoteMotionStep\[\]/, "Group stream must expose exec_group messages");
assert.match(sharedProtocol, /type: "exec_group"[\s\S]*timeoutMs\?: number/, "Group stream exec_group must allow renderer-specified ACK timeout");
assert.match(sharedProtocol, /kind: "move_xy"[\s\S]*dxMm: number[\s\S]*dyMm: number/, "move_xy must carry relative machine deltas");
assert.match(sharedProtocol, /type: "group_done"[\s\S]*groupId: string/, "Group DONE must identify the completed group");
assert.match(sharedProtocol, /motors\?: Array<\{[\s\S]*readiness: MotorReadiness/, "Group stream telemetry motors must include readiness");
assert.doesNotMatch(sharedProtocol, /export type PrimitiveCommand|type: "command"|op: "move_to"/, "Shared group stream protocol must not expose primitive command messages");

const keymapDomain = readFileSync(new URL("../../../apps/desktop/src/domain/keymap.ts", import.meta.url), "utf8");
assert.match(keymapDomain, /if \(base\?\.bindings && base\.bindings\.length > 0\)/, "Desktop keymap must preserve existing device bindings");
const groupStreamPlanner = readFileSync(new URL("../../../apps/desktop/src/domain/groupStreamPlanner.ts", import.meta.url), "utf8");
assert.match(groupStreamPlanner, /kind: "move_xy"[\s\S]*dxMm: target\.xMm - current\.xMm[\s\S]*dyMm: target\.yMm - current\.yMm/, "Desktop planner must emit relative machine deltas");
assert.doesNotMatch(groupStreamPlanner, /toSvgCoords|svgY|bbox\.maxY|RemoteMotionBlock|blocks:/, "Desktop planner must not use SVG coordinates or legacy block payloads");
assert.match(groupStreamPlanner, /kind: "move_xy"[\s\S]*kind: "servo_press"[\s\S]*kind: "servo_release"/, "Text planning must create core remote motion steps");
assert.match(groupStreamPlanner, /!\s*disableLineFeed\s*\?\s*\(\[\{ kind: "character_release" \}\]/, "Line-feed degraded planning must skip character_release steps");
assert.match(groupStreamPlanner, /已启用跳过走纸模式，跳过字符释放和换行走纸/, "Line-feed degraded planning must warn when skipping feed actions");
assert.match(groupStreamPlanner, /!disableLineFeed && !appendGroup\("line_feed", \[\{ kind: "line_feed" \}\]/, "Line-feed degraded planning must skip line_feed steps");
assert.doesNotMatch(groupStreamPlanner, /line_feed_disabled/, "Line-feed degraded planning must not reject newline tasks");
assert.match(groupStreamPlanner, /kind: "line_feed"/, "Newline planning must create line_feed step");
assert.match(groupStreamPlanner, /`\$\{jobId\}-\$\{String\(groupIndex\)\.padStart\(4, "0"\)\}-\$\{kind\}`/, "Desktop planner must create deterministic group ids");

const electronMain = readFileSync(new URL("../../../apps/desktop/electron/main.ts", import.meta.url), "utf8");
const deviceLink = readFileSync(new URL("../../../apps/desktop/electron/device-link.ts", import.meta.url), "utf8");
const groupStreamClient = readFileSync(new URL("../../../apps/desktop/src/domain/groupStreamClient.ts", import.meta.url), "utf8");
assert.match(electronMain, /from "node:http"/, "Electron main network transport must use Node http");
assert.match(electronMain, /from "node:https"/, "Electron main network transport must support https");
assert.match(deviceLink, /from "node:net"/, "DeviceLink must use Node net for TCP");
assert.match(electronMain, /function networkErrorResponse/, "Electron main must build structured network errors");
assert.match(electronMain, /status:\s*0[\s\S]*statusText:\s*"network_error"/, "Network failures must return status 0 network_error");
assert.match(electronMain, /cause.*message/, "Network error details must include cause message extraction");
assert.match(electronMain, /code.*syscall/s, "Network error details must include code and syscall extraction");
assert.match(electronMain, /console\.error\(`\[network:request\]/, "Network failures must be logged in Electron main");
assert.match(electronMain, /function normalizeHeaders/, "Electron main must normalize request headers");
assert.match(electronMain, /Accept:\s*"application\/json"/, "Electron main must send Accept application/json");
assert.match(electronMain, /Connection:\s*"close"/, "Electron main must request short-lived HTTP connections");
assert.match(electronMain, /agent:\s*false/, "Electron main must disable Node HTTP connection reuse");
assert.match(electronMain, /timeout:\s*networkRequestTimeoutMs/, "Electron main must set a request timeout");
assert.match(electronMain, /req\.destroy\(new Error\("Request timed out"\)\)/, "Electron timeout must destroy the request");
assert.match(electronMain, /Content-Length/, "Electron main must send Content-Length for request bodies");
assert.doesNotMatch(
  electronMain,
  /fetch\(request\.url,\s*request\.init\)/,
  "Electron main network IPC must not use raw fetch",
);
assert.doesNotMatch(
  electronMain,
  /POST \/api\/jobs[\s\S]*retry|retry[\s\S]*POST \/api\/jobs/,
  "POST /api/jobs must not gain automatic retry without idempotency",
);
assert.doesNotMatch(deviceLink, /magic0|magic1|headerLength|frameTypes|writeFrame|pushData/, "Desktop TCP protocol must not use binary frames");
assert.match(deviceLink, /JSON\.stringify\(message\)\}\\n/, "DeviceLink must write NDJSON lines");
assert.match(deviceLink, /maxLineBytes\s*=\s*4096/, "DeviceLink must cap inbound NDJSON lines");
assert.match(deviceLink, /replace\(\/\\r\/g,\s*""\)/, "DeviceLink must ignore carriage returns");
assert.match(deviceLink, /type: "hello"[\s\S]*client: "desktop"/, "DeviceLink must send hello");
assert.match(deviceLink, /type: "exec_group"/, "DeviceLink must support exec_group");
assert.doesNotMatch(deviceLink, /exec_block|block_started|block_done|BlockStream|RemoteMotionBlock|execBlock/, "DeviceLink must not retain legacy block stream protocol");
assert.doesNotMatch(deviceLink, /type: "command"|op:/, "DeviceLink must not send primitive commands");
assert.match(electronMain, /\["exec_group", "cancel", "reset_fault", "probe", "ping"\]/, "Electron IPC must allow group stream top-level commands");
assert.doesNotMatch(electronMain, /exec_block|blockStream|BlockStream|Block stream/, "Electron main must not retain legacy block stream IPC");
assert.match(electronMain, /delete outbound\.timeoutMs/, "Electron IPC must not forward local timeoutMs to firmware");
assert.match(electronMain, /deviceLink\.sendCommand\(outbound,\s*timeoutMs\)/, "Electron IPC must pass renderer-specified group stream timeouts");
assert.doesNotMatch(electronMain, /message\.type !== "command"|Only primitive commands/, "Electron IPC must not require primitive command messages");
assert.match(deviceLink, /ackTimeoutMs\s*=\s*1000/, "DeviceLink commands must have a 1000ms ACK timeout");
assert.match(deviceLink, /helloTimeoutMs\s*=\s*5000/, "DeviceLink hello must allow a 5000ms ACK timeout");
assert.match(deviceLink, /execGroupAckTimeoutMs\s*=\s*5000/, "DeviceLink exec_group must allow a 5000ms ACK timeout");
assert.match(deviceLink, /message\.type === "exec_group"[\s\S]*return execGroupAckTimeoutMs/, "DeviceLink must use the exec_group ACK timeout");
assert.match(deviceLink, /Group stream hello ack timed out after/, "DeviceLink hello timeout must report handshake context");
assert.match(deviceLink, /Group stream hello rejected:/, "DeviceLink hello faults must surface firmware details");
assert.match(deviceLink, /Group stream \$\{type\} ack timed out after/, "DeviceLink exec timeout must report command context");
assert.match(deviceLink, /this\.buffer \+= chunk\.toString/, "DeviceLink must buffer partial TCP chunks");
assert.match(electronMain, /Group stream disconnected/, "Group stream disconnect must be surfaced to the UI");
assert.match(groupStreamClient, /execGroupAckTimeoutMs\s*=\s*5000/, "Renderer group stream client must request a 5000ms exec_group ACK timeout");
assert.match(groupStreamClient, /type: "exec_group"[\s\S]*timeoutMs: execGroupAckTimeoutMs/, "Renderer exec_group messages must carry explicit ACK timeout");
assert.match(groupStreamClient, /Group \$\{groupId\} exec_group failed/, "Renderer exec_group failures must identify the group");
assert.doesNotMatch(groupStreamClient, /sendExecBlock|exec_block|BlockStream|RemoteMotionBlock|blockId/, "Renderer group stream client must not retain legacy block execution");

const deviceClient = readFileSync(new URL("../../../apps/desktop/src/domain/deviceClient.ts", import.meta.url), "utf8");
assert.match(deviceClient, /catch \(error\)[\s\S]*networkErrorMessage/, "DeviceClient must defensively catch request failures");
assert.match(deviceClient, /result\.status\s*===\s*0|parsed\.code === "network_error"/, "DeviceClient must handle structured network_error responses");
assert.match(deviceClient, /details\?\.code[\s\S]*details\?\.syscall/, "DeviceClient network errors must include code/syscall in messages");
assert.match(deviceClient, /wifiNetworks\(\)/, "DeviceClient must expose WiFi network scans");
assert.match(deviceClient, /configureWifi\(request: WifiConfigRequest\)/, "DeviceClient must expose WiFi setup config");

assert.match(httpServer, /kMaxJobRequestBytes\s*=\s*8192/, "Create job must cap request bodies at 8192 bytes");
assert.doesNotMatch(httpServer, /void handleCreateJob\(\)\s*\{[\s\S]*StaticJsonDocument<512>/, "Create job must not use StaticJsonDocument<512>");
assert.match(httpServer, /DynamicJsonDocument request\(body\.length\(\) \+ 512\)/, "Create job must size JSON parsing from body length");
assert.match(httpServer, /body\.length\(\) > kMaxJobRequestBytes/, "Create job must reject oversized bodies");
assert.match(httpServer, /sendError\(413,\s*"job_too_large"/, "Oversized job requests must return job_too_large");
assert.match(httpServer, /sendHeader\("Connection",\s*"close"\)/, "Firmware JSON responses must close HTTP connections");
assert.match(httpServer, /sendHeader\("Cache-Control",\s*"no-store"\)/, "Firmware JSON responses must disable response caching");
assert.doesNotMatch(httpServer, /server_\.client\(\)\.stop\(\)/, "Firmware must not immediately hard-stop response clients");
assert.match(httpServer, /\[http\] response status=/, "Firmware JSON responses must log response status");
assert.match(httpServer, /json\.length\(\)/, "Firmware JSON responses must log response byte length");
assert.match(httpServer, /\[http\] POST \/api\/jobs bodyBytes=/, "Create job must log request body bytes");
assert.match(httpServer, /\[http\] POST \/api\/jobs invalid_json/, "Create job must log JSON parse errors");
assert.match(httpServer, /\[http\] POST \/api\/jobs textLength=/, "Create job must log text length");
assert.match(httpServer, /\[http\] POST \/api\/jobs missing text/, "Create job must log missing text");
assert.match(httpServer, /\[http\] POST \/api\/jobs accepted=/, "Create job must log submit result");
assert.match(httpServer, /\[http\] POST \/api\/jobs response sent/, "Create job must log response completion");
assert.match(keymapDomain, /return sanitizeKeymap\(\{[\s\S]*bindings: base\.bindings/, "Desktop keymap preservation path must sanitize existing bindings");

const appTsx = readFileSync(new URL("../../../apps/desktop/src/ui/App.tsx", import.meta.url), "utf8");
assert.match(appTsx, /sanitizeKeymap\(await client\.putKeymap\(nextKeymap\)\)/, "Keymap sync must upload and store sanitized current bindings");
assert.match(appTsx, /planTextToRemoteMotionGroups/, "Print Task must plan text locally");
assert.match(appTsx, /client\.probeMotors\(\)[\s\S]*planTextToRemoteMotionGroups/, "Print Task must refresh HTTP motor readiness before planning");
assert.match(appTsx, /isLineFeedMotorReady\(latestStatus\)/, "Print Task must detect line-feed availability from refreshed motor status");
assert.match(appTsx, /const disableLineFeed = skipLineFeed \|\| !lineFeedAvailable/, "Print Task must support manual and automatic line-feed skipping");
assert.match(appTsx, /checked=\{skipLineFeed\}/, "Print Task must expose a skip line-feed mode");
assert.match(appTsx, /mergeTelemetryMotors/, "Print Task must merge group stream motor telemetry into device status");
assert.match(appTsx, /streamClient\.probe\(\)/, "Print Task must refresh motor feedback before sending group stream commands");
assert.match(appTsx, /Group probe rejected/, "Print Task must surface rejected group stream probes");
assert.match(appTsx, /streamClient\.sendExecGroup/, "Print Task must send exec_group messages over TCP");
assert.match(appTsx, /message\.type === "group_done"[\s\S]*message\.groupId === planned\.groupId/, "Print Task must wait for group_done before sending the next group");
assert.match(appTsx, /groupDoneTimeoutMs\(planned\)/, "Print Task must time out missing group_done events");
assert.doesNotMatch(appTsx, /block_done|block_started|sendExecBlock|BlockStream|totalBlocks|planned\.blocks|Block probe|块流/, "Print Task UI must not retain legacy block stream code");
assert.doesNotMatch(appTsx, /client\.createJob/, "Print Task must not call POST /api/jobs");
assert.match(appTsx, /<TaskStatusPanel status=\{status\} logLines=\{logLines\} printTask=\{printTask\} \/>/, "Print Task page must show group stream task state");
assert.match(appTsx, /<WifiSetupPanel/, "Settings page must expose WiFi setup controls");
assert.match(appTsx, /setupClient\.wifiNetworks\(\)/, "WiFi setup UI must scan SSIDs through the ESP32");
assert.match(appTsx, /<select value=\{selectedSsid\}/, "WiFi setup UI must choose SSID from a select control");
assert.match(appTsx, /setupClient\.configureWifi/, "WiFi setup UI must submit selected credentials");
assert.match(appTsx, /finishWifiSetup/, "WiFi setup UI must close setup AP after successful connection");

const groupCommandServer = readFileSync(new URL("../transport/GroupCommandServer.h", import.meta.url), "utf8");
const groupCommandProtocol = readFileSync(new URL("../transport/GroupCommandProtocol.h", import.meta.url), "utf8");
const autoTyperIno = readFileSync(new URL("../auto_typer.ino", import.meta.url), "utf8");
assert.match(groupCommandServer, /kGroupCommandPort\s*=\s*7777/, "Group command server must listen on TCP 7777");
assert.match(groupCommandServer, /lineBuffer_ \+= static_cast<char>\(value\)/, "Group command server must buffer NDJSON input");
assert.match(groupCommandServer, /client\.write\('\\n'\)/, "Group command server must write NDJSON newlines");
assert.match(groupCommandServer, /doc\["ok"\]\s*=\s*accepted/, "Group command ACK must include ok");
assert.match(groupCommandServer, /strcmp\(type,\s*"ping"\)/, "Group command server must handle ping");
assert.match(groupCommandServer, /sendPong\(id\)/, "Group command server must reply to ping with pong");
assert.match(groupCommandServer, /strcmp\(type,\s*"probe"\)[\s\S]*app_\.probeMotors\(\)/, "Group command probe must refresh motor feedback");
assert.match(groupCommandServer, /probe_rejected/, "Group command probe must reject when motor probing is unavailable");
assert.match(groupCommandServer, /motor\["readiness"\]\s*=\s*motorReadinessText\(state\.readiness\)/, "Group command telemetry must report motor readiness");
assert.match(groupCommandProtocol, /isfinite\(out\.dxMm\)[\s\S]*isfinite\(out\.dyMm\)/, "Group command protocol must reject non-finite move_xy deltas");
assert.match(groupCommandProtocol, /parseRemoteGroup/, "Group command protocol must parse exec_group steps");
assert.match(groupCommandProtocol, /group_too_large/, "Group command protocol must reject oversized exec_group commands");
assert.match(groupCommandServer, /handleExecGroup/, "Group command server must handle exec_group");
assert.match(groupCommandServer, /RemoteMotionStep steps\[kRemoteGroupMaxSteps\]/, "Group command server must cap group stack storage");
assert.match(groupCommandServer, /submitRemoteGroup\(steps,\s*count,\s*groupId\)/, "Group command server must submit parsed remote groups");
assert.match(groupCommandServer, /sendGroupDone/, "Group command server must emit group_done events");
assert.doesNotMatch(groupCommandServer, /exec_block|sendBlock|block_started|block_done|BlockCommand|kBlockCommand|currentBlockId|lastCompletedBlockId|FrameCodec|parseDesktopCommand|raw_can|can_frame|twai_transmit/, "Group command server must not use legacy block commands, binary frames, or expose raw CAN");
assert.doesNotMatch(groupCommandProtocol, /RemoteMotionBlock|parseRemoteBlock|invalid_block|blocks array/, "Group command protocol must not expose legacy remote block parsing");
assert.match(autoTyperIno, /#include "transport\/GroupCommandServer\.h"/, "Sketch must include GroupCommandServer");
assert.match(autoTyperIno, /GroupCommandServer gGroupServer/, "Sketch must instantiate GroupCommandServer");
assert.match(autoTyperIno, /gGroupServer\.begin\(\)/, "Sketch must start GroupCommandServer");
assert.match(autoTyperIno, /WiFi\.setSleep\(false\)/, "Sketch must disable Wi-Fi sleep for TCP execution");
assert.match(autoTyperIno, /gHttp\.tick\(\)[\s\S]*gGroupServer\.tick\(\)[\s\S]*gApp\.tick\(\)[\s\S]*delay\(1\)/, "Sketch loop must tick HTTP, group server, app, and use delay(1)");
assert.match(autoTyperRuntime, /submitRemoteGroup/, "Firmware must expose remote group submission");
assert.match(autoTyperRuntime, /count > kRemoteGroupMaxSteps/, "Firmware must cap remote group size");
assert.match(autoTyperRuntime, /MachinePointMm plannedPoint = remoteCurrentPoint_/, "Remote group conversion must track a local planned point");
assert.match(autoTyperRuntime, /convertRemoteStep\(steps\[i\], motionStep, result, plannedPoint\)/, "Remote group conversion must pass the planned point to each step");
assert.match(autoTyperRuntime, /plannedPoint = motionStep\.targetMm/, "Remote group conversion must advance planned point after position-changing steps");
assert.match(autoTyperRuntime, /consumeRemoteGroupStarted/, "Firmware must expose remote group started events");
assert.match(autoTyperRuntime, /consumeRemoteGroupDone/, "Firmware must expose remote group done events");
assert.match(autoTyperRuntime, /convertRemoteStep/, "Firmware must convert remote steps before execution");
assert.doesNotMatch(autoTyperRuntime, /submitRemoteBlock|consumeRemoteBlock|RemoteMotionBlock|RemoteMotionBlockKind|SubmitRemoteBlock|invalid_block/, "Firmware remote protocol must not retain legacy remote block API");
assert.match(autoTyperRuntime, /actuatorProbeMayRecover/, "Remote group submission must retry stale actuator readiness after probing");
assert.match(autoTyperRuntime, /probeMotorsBestEffort\(400\)[\s\S]*required = checkRequiredActuators\(activeMotionSteps_\)/, "Remote group submission must recheck required actuators after probing");
assert.doesNotMatch(autoTyperRuntime, /MotionStepPlan single/, "Remote group submission must not allocate a full step plan on the loop stack");
assert.match(autoTyperRuntime, /executor_\.start\(activeMotionSteps_\.steps,\s*activeMotionSteps_\.count,\s*startPoint\)/, "Remote groups must start through MotionExecutor");
assert.match(autoTyperRuntime, /xyDeltaSteps\(currentPoint,\s*target,\s*config_\.calibration\)/, "Remote move_to must use firmware kinematics");

function pick(object, keys) {
  return Object.fromEntries(keys.map((key) => [key, object[key]]));
}

console.log("firmware planner/kinematics regression tests passed");
