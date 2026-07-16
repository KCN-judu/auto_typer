import assert from "node:assert/strict";
import {
  atomicMotionBlock,
  encodeLineFeedMove,
  encodeLineFeedHome,
  encodePressMove,
  encodeReturnToZero,
  encodeXyMove,
  initialMotionPosition,
} from "../src/domain/planner/absoluteMotionEncoder.js";

const firstXy = encodeXyMove(initialMotionPosition(), 100, 50, 5000);
assert.ok(firstXy);
assert.deepEqual(firstXy.block.map((move) => move.type === "motor_move" ? [move.motorId, move.target] : []), [
  [1, 100],
  [2, -50],
  [3, 50],
]);

const secondXy = encodeXyMove(firstXy.position, 60, 75, 5000);
assert.ok(secondXy);
assert.deepEqual(secondXy.block.map((move) => move.type === "motor_move" ? [move.motorId, move.target] : []), [
  [1, 60],
  [2, -75],
  [3, 75],
]);
assert.equal(encodeXyMove(secondXy.position, 60, 75, 5000), undefined);

const firstLineFeed = encodeLineFeedMove(secondXy.position, -180);
const secondLineFeed = encodeLineFeedMove(firstLineFeed.position, -360);
assert.equal(firstLineFeed.block[0].type === "motor_move" && firstLineFeed.block[0].target, -180);
assert.equal(secondLineFeed.block[0].type === "motor_move" && secondLineFeed.block[0].target, -360);

const lineFeedHome = encodeLineFeedHome(secondLineFeed.position);
assert.deepEqual(lineFeedHome.map((encoded) => encoded.block[0].type === "motor_move" && encoded.block[0].target), [16400, 10000]);

const pressDown = encodePressMove(secondLineFeed.position, -2700);
const pressUp = encodePressMove(pressDown.position, 0);
assert.equal(pressDown.block[0].type === "motor_move" && pressDown.block[0].target, -2700);
assert.equal(pressUp.block[0].type === "motor_move" && pressUp.block[0].target, 0);

const returnBlocks = encodeReturnToZero({ x: 120, y: -40, l: -360, z: -2700 });
assert.deepEqual(
  returnBlocks.flatMap((encoded) => encoded.block.map((action) => action.type === "motor_move" ? [action.motorId, action.target] : [])),
  [[1, 0], [2, 0], [3, 0], [4, 16400], [4, 10000], [5, 0]],
);
assert.deepEqual(returnBlocks.at(-1)?.position, { x: 0, y: 0, l: 10000, z: 0 });
assert.throws(() => atomicMotionBlock([]), /must not be empty/);

console.log("absolute motion encoder tests passed");
