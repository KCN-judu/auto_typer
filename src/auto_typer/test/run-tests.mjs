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

const httpServer = readFileSync(new URL("../http_control_server.h", import.meta.url), "utf8");
assert.match(httpServer, /case JobState::None:[\s\S]*return "none";/, "JobState::None must serialize to none");
assert.match(httpServer, /request\["point"\]/, "ProbeKeyRequest must read nested point");
assert.doesNotMatch(httpServer, /extractString|extractFloat|extractInt/, "HTTP handlers must not use ad-hoc JSON extractors");

console.log("firmware planner/kinematics regression tests passed");
