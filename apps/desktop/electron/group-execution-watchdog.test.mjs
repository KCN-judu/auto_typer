import assert from "node:assert/strict";
import {
  GroupExecutionWatchdog,
} from "../dist-test/apps/desktop/src/domain/groupExecutionWatchdog.js";

function createGroup(blockCount = 32) {
  return {
    groupId: "g-1",
    seq: 7,
    policy: { maxRuntimeMs: 30000, onDisconnect: "cancel" },
    blocks: Array.from({ length: blockCount }, () => ({ type: "wait", durationMs: 1 })),
    absoluteMaxRuntimeMs: 120000,
  };
}

function started(group) {
  return { v: 1, type: "group_started", groupId: group.groupId, seq: group.seq };
}

function blockStarted(group, blockIndex, blockType = "wait") {
  return { v: 1, type: "block_started", groupId: group.groupId, seq: group.seq, blockIndex, blockType };
}

function blockDone(group, blockIndex, blockType = "wait") {
  return { v: 1, type: "block_done", groupId: group.groupId, seq: group.seq, blockIndex, blockType };
}

function testProgressEveryTwoSecondsDoesNotTimeoutBeforeFinal() {
  const group = createGroup(32);
  const watchdog = new GroupExecutionWatchdog(group, 0);
  assert.equal(watchdog.observe(started(group), 0).kind, "progress");

  for (let index = 0; index < 31; index += 1) {
    const now = (index + 1) * 2000;
    watchdog.observe(blockStarted(group, index), now);
    watchdog.observe(blockDone(group, index), now + 500);
    assert.equal(watchdog.checkTimeout(now + 1500), undefined);
  }

  assert.equal(watchdog.checkTimeout(62000), undefined, "progress past 10s must not trip a fixed execution timeout");
  watchdog.observe(blockStarted(group, 31), 64000);
  watchdog.observe(blockDone(group, 31), 64500);
  const final = watchdog.observe({ v: 1, type: "group_final", groupId: group.groupId, seq: group.seq, status: "done" }, 65000);
  assert.deepEqual(final, { kind: "final", outcome: { status: "done", code: undefined, message: undefined, eventType: "group_final" } });
}

function testFinalTimeoutStartsOnlyAfterLastExpectedBlockDone() {
  const group = createGroup(32);
  const watchdog = new GroupExecutionWatchdog(group, 0, { finalAfterLastBlockTimeoutMs: 4000 });
  watchdog.observe(started(group), 0);
  watchdog.observe(blockDone(group, 30), 1000);
  assert.equal(watchdog.checkTimeout(5001), undefined, "final timeout must not start before the last block_done");

  watchdog.observe(blockDone(group, 31), 6000);
  const timeout = watchdog.checkTimeout(10001);
  assert.equal(timeout?.timeoutKind, "final_timeout");
  assert.equal(timeout?.blockIndex, 31);
}

function testProgressTimeoutWhenProgressStops() {
  const group = createGroup(4);
  const watchdog = new GroupExecutionWatchdog(group, 0, { progressTimeoutMs: 12000 });
  watchdog.observe(started(group), 0);
  const timeout = watchdog.checkTimeout(12001);
  assert.equal(timeout?.timeoutKind, "progress_timeout");
  assert.equal(timeout?.lastProgressEvent, "group_started");
}

function testNoFixedTenSecondTimeoutForRunningGroup() {
  const group = createGroup(8);
  const watchdog = new GroupExecutionWatchdog(group, 0);
  watchdog.observe(started(group), 0);
  watchdog.observe(blockStarted(group, 0), 2000);
  watchdog.observe(blockDone(group, 0), 4000);
  watchdog.observe(blockStarted(group, 1), 6000);
  watchdog.observe(blockDone(group, 1), 8000);
  watchdog.observe(blockStarted(group, 2), 10000);
  assert.equal(watchdog.checkTimeout(10001), undefined);
}

function testGroupDoneCompatibilityFinal() {
  const group = createGroup(1);
  const watchdog = new GroupExecutionWatchdog(group, 0);
  const observed = watchdog.observe({ v: 1, type: "group_done", groupId: group.groupId, seq: group.seq, ok: true }, 1);
  assert.deepEqual(observed, { kind: "final", outcome: { status: "done", eventType: "group_done" } });
}

function testFaultWithGroupIdIsFailedFinal() {
  const group = createGroup(1);
  const watchdog = new GroupExecutionWatchdog(group, 0);
  const observed = watchdog.observe({
    v: 1,
    type: "fault",
    groupId: group.groupId,
    seq: group.seq,
    code: "motion_feedback_timeout",
    message: "Motion feedback timed out",
  }, 1);
  assert.deepEqual(observed, {
    kind: "final",
    outcome: {
      status: "failed",
      code: "motion_feedback_timeout",
      message: "Motion feedback timed out",
      eventType: "fault",
    },
  });
}

function testCancelledFinalIsCancelled() {
  const group = createGroup(1);
  const watchdog = new GroupExecutionWatchdog(group, 0);
  const observed = watchdog.observe({
    v: 1,
    type: "group_final",
    groupId: group.groupId,
    seq: group.seq,
    status: "cancelled",
    code: "cancelled",
    message: "Remote group cancelled",
  }, 1);
  assert.deepEqual(observed, {
    kind: "final",
    outcome: {
      status: "cancelled",
      code: "cancelled",
      message: "Remote group cancelled",
      eventType: "group_final",
    },
  });
}

testProgressEveryTwoSecondsDoesNotTimeoutBeforeFinal();
testFinalTimeoutStartsOnlyAfterLastExpectedBlockDone();
testProgressTimeoutWhenProgressStops();
testNoFixedTenSecondTimeoutForRunningGroup();
testGroupDoneCompatibilityFinal();
testFaultWithGroupIdIsFailedFinal();
testCancelledFinalIsCancelled();

console.log("group execution watchdog tests passed");
