import type { KeymapDocument, MachinePointMm } from "../../../../shared/protocol/protocolTypes";
import { displayKey } from "./keymap";

// ─── Visual Node ─────────────────────────────────────────────────────────────

export interface KeymapVisualNode {
  /** Index in the bindings array */
  index: number;
  /** Normalized key string */
  key: string;
  /** Display label for the node */
  label: string;
  /** Machine coordinate X in mm */
  xMm: number;
  /** Machine coordinate Y in mm */
  yMm: number;
}

// ─── Bounding Box ────────────────────────────────────────────────────────────

export interface BBox {
  minX: number;
  maxX: number;
  minY: number;
  maxY: number;
  width: number;
  height: number;
}

// ─── Build Visual Nodes ──────────────────────────────────────────────────────

export function buildVisualNodes(keymap: KeymapDocument): KeymapVisualNode[] {
  return keymap.bindings.map((binding, index) => ({
    index,
    key: binding.key,
    label: displayKey(binding.key),
    xMm: binding.point.xMm,
    yMm: binding.point.yMm,
  }));
}

// ─── Compute Bounding Box ────────────────────────────────────────────────────

export function computeBBox(nodes: KeymapVisualNode[]): BBox | null {
  if (nodes.length === 0) return null;

  let minX = Infinity;
  let maxX = -Infinity;
  let minY = Infinity;
  let maxY = -Infinity;

  for (const node of nodes) {
    if (node.xMm < minX) minX = node.xMm;
    if (node.xMm > maxX) maxX = node.xMm;
    if (node.yMm < minY) minY = node.yMm;
    if (node.yMm > maxY) maxY = node.yMm;
  }

  return {
    minX,
    maxX,
    minY,
    maxY,
    width: maxX - minX,
    height: maxY - minY,
  };
}

// ─── SVG ViewBox ────────────────────────────────────────────────────────────

export interface SvgViewBox {
  width: number;
  height: number;
}

/**
 * Compute SVG viewBox dimensions with padding.
 * The SVG coordinate space starts at (0, 0) in the top-left.
 * Machine Y is flipped when rendering (see toSvgCoords).
 */
export function computeSvgViewBox(bbox: BBox, paddingMm: number = 8): SvgViewBox {
  return {
    width: bbox.width + paddingMm * 2,
    height: bbox.height + paddingMm * 2,
  };
}

// ─── Coordinate Transform ───────────────────────────────────────────────────

export interface SvgCoords {
  svgX: number;
  svgY: number;
}

/**
 * Transform machine coordinates to SVG coordinates.
 *
 * Machine: origin bottom-left, Y+ up.
 * SVG: origin top-left, Y+ down.
 *
 * svgX = (xMm - minX) + padding
 * svgY = (maxY - yMm) + padding   ← flips Y
 */
export function toSvgCoords(xMm: number, yMm: number, bbox: BBox, paddingMm: number = 8): SvgCoords {
  return {
    svgX: (xMm - bbox.minX) + paddingMm,
    svgY: (bbox.maxY - yMm) + paddingMm,
  };
}

// ─── Search ──────────────────────────────────────────────────────────────────

/**
 * Returns true if the node matches the search query.
 * Matches by character label (case-insensitive).
 */
export function matchesSearch(node: KeymapVisualNode, query: string): boolean {
  if (!query) return true;
  const q = query.toLowerCase();
  // Match by key character
  if (node.key.toLowerCase().includes(q)) return true;
  // Match by display label (e.g. "Space")
  if (node.label.toLowerCase().includes(q)) return true;
  return false;
}

// ─── Head Position ───────────────────────────────────────────────────────────

export interface HeadPosition {
  xMm: number;
  yMm: number;
}

/**
 * Extract current head position from device status if available.
 * Returns null if no job is running or no coordinates are present.
 */
export function extractHeadPosition(currentPoint?: MachinePointMm): HeadPosition | null {
  if (!currentPoint) return null;
  if (!Number.isFinite(currentPoint.xMm) || !Number.isFinite(currentPoint.yMm)) return null;
  return { xMm: currentPoint.xMm, yMm: currentPoint.yMm };
}
