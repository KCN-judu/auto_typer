import { useMemo, useState } from "react";
import type { DeviceStatus, KeymapDocument } from "../../../../../shared/protocol/auto-typer-protocol";
import {
  buildVisualNodes,
  computeBBox,
  computeSvgViewBox,
  extractHeadPosition,
  matchesSearch,
  toSvgCoords,
} from "../../domain/keymapLayout";
import type { KeymapVisualNode } from "../../domain/keymapLayout";
import { KeymapInspector } from "./KeymapInspector";
import { KeymapLegend } from "./KeymapLegend";

const PADDING_MM = 8;

interface KeymapPageProps {
  keymap: KeymapDocument;
  status: DeviceStatus;
}

export function KeymapPage({ keymap, status }: KeymapPageProps) {
  const [search, setSearch] = useState("");
  const [hoveredNode, setHoveredNode] = useState<KeymapVisualNode | null>(null);
  const [selectedNode, setSelectedNode] = useState<KeymapVisualNode | null>(null);

  const nodes = useMemo(() => buildVisualNodes(keymap), [keymap]);
  const bbox = useMemo(() => computeBBox(nodes), [nodes]);
  const svgViewBox = useMemo(() => (bbox ? computeSvgViewBox(bbox, PADDING_MM) : null), [bbox]);

  const headPosition = useMemo(
    () => extractHeadPosition(status.currentJob?.currentPoint),
    [status.currentJob?.currentPoint],
  );

  const inspectedNode = selectedNode ?? hoveredNode;

  if (nodes.length === 0 || !bbox || !svgViewBox) {
    return (
      <section className="kmPage">
        <div className="kmEmpty">暂无映射数据，请先连接设备并同步映射表</div>
      </section>
    );
  }

  return (
    <section className="kmPage">
      <div className="kmMain">
        <div className="kmToolbar">
          <input
            className="kmSearchInput"
            type="text"
            placeholder="搜索按键..."
            value={search}
            onChange={(e) => setSearch(e.target.value)}
          />
          {search && (
            <span className="kmSearchHint">
              {nodes.filter((n) => matchesSearch(n, search)).length} / {nodes.length} 匹配
            </span>
          )}
        </div>

        <div className="kmCanvasWrap">
          <svg
            className="kmCanvasSvg"
            viewBox={`0 0 ${svgViewBox.width} ${svgViewBox.height}`}
            preserveAspectRatio="xMidYMid meet"
          >
            {/* Grid lines for reference */}
            <GridLines svgViewBox={svgViewBox} bbox={bbox} paddingMm={PADDING_MM} />

            {/* Key nodes */}
            {nodes.map((node) => {
              const { svgX, svgY } = toSvgCoords(node.xMm, node.yMm, bbox, PADDING_MM);
              const isMatch = matchesSearch(node, search);
              const isHighlight = search ? isMatch : false;
              const isSelected = selectedNode?.index === node.index;
              const isMuted = search ? !isMatch : false;

              let className = "kmNode";
              if (isMuted) className += " muted";
              if (isHighlight) className += " highlight";
              if (isSelected) className += " selected";

              return (
                <g
                  key={node.index}
                  className={className}
                  onMouseEnter={() => setHoveredNode(node)}
                  onMouseLeave={() => setHoveredNode(null)}
                  onClick={() => setSelectedNode(isSelected ? null : node)}
                >
                  <circle className="kmNodeDot" cx={svgX} cy={svgY} r={1.8} />
                  <text
                    className="kmNodeLabel"
                    x={svgX}
                    y={svgY - 3}
                    textAnchor="middle"
                  >
                    {node.label}
                  </text>
                </g>
              );
            })}

            {/* Current head position overlay */}
            {headPosition && (
              <HeadOverlay xMm={headPosition.xMm} yMm={headPosition.yMm} bbox={bbox} paddingMm={PADDING_MM} />
            )}

            {/* Axis indicator at bottom-left */}
            <AxisIndicator svgViewBox={svgViewBox} />
          </svg>
        </div>
      </div>

      <div className="kmSidebar">
        <KeymapInspector node={inspectedNode} />
        <KeymapLegend />
      </div>
    </section>
  );
}

// ── Head Position Overlay ────────────────────────────────────────────────────

function HeadOverlay({ xMm, yMm, bbox, paddingMm }: { xMm: number; yMm: number; bbox: { minX: number; maxX: number; minY: number; maxY: number; width: number; height: number }; paddingMm: number }) {
  const { svgX, svgY } = toSvgCoords(xMm, yMm, bbox, paddingMm);
  return (
    <g>
      <line className="kmHeadCrosshair" x1={svgX - 6} y1={svgY} x2={svgX + 6} y2={svgY} />
      <line className="kmHeadCrosshair" x1={svgX} y1={svgY - 6} x2={svgX} y2={svgY + 6} />
      <circle className="kmHeadDot" cx={svgX} cy={svgY} r={1.2} />
    </g>
  );
}

// ── Axis Indicator (bottom-left) ─────────────────────────────────────────────

function AxisIndicator({ svgViewBox }: { svgViewBox: { width: number; height: number } }) {
  // Position the axis indicator near the bottom-left corner
  const originX = 12;
  const originY = svgViewBox.height - 12;
  const arrowLen = 14;

  return (
    <g className="kmAxisIndicator">
      {/* X+ arrow (right) */}
      <line x1={originX} y1={originY} x2={originX + arrowLen} y2={originY} stroke="var(--km-node)" strokeWidth="0.6" />
      <polygon
        points={`${originX + arrowLen},${originY} ${originX + arrowLen - 2},${originY - 1} ${originX + arrowLen - 2},${originY + 1}`}
        fill="var(--km-node)"
      />
      <text x={originX + arrowLen + 1.5} y={originY + 1} className="kmAxisLabel">X+ / CW</text>

      {/* Y+ arrow (up) */}
      <line x1={originX} y1={originY} x2={originX} y2={originY - arrowLen} stroke="var(--km-node)" strokeWidth="0.6" />
      <polygon
        points={`${originX},${originY - arrowLen} ${originX - 1},${originY - arrowLen + 2} ${originX + 1},${originY - arrowLen + 2}`}
        fill="var(--km-node)"
      />
      <text x={originX + 1.5} y={originY - arrowLen - 1} className="kmAxisLabel">Y+ / CCW</text>

      {/* Origin dot */}
      <circle cx={originX} cy={originY} r={0.8} fill="var(--km-node)" />
    </g>
  );
}

// ── Grid Lines (subtle reference grid) ───────────────────────────────────────

function GridLines({ svgViewBox, bbox, paddingMm }: { svgViewBox: { width: number; height: number }; bbox: { minX: number; maxX: number; minY: number; maxY: number; width: number; height: number }; paddingMm: number }) {
  const step = 20; // 20mm grid in machine space
  const lines: { x1: number; y1: number; x2: number; y2: number; key: string }[] = [];

  // Vertical lines (constant X in machine space)
  const startX = Math.floor(bbox.minX / step) * step;
  const endX = Math.ceil(bbox.maxX / step) * step;
  for (let machineX = startX; machineX <= endX; machineX += step) {
    const { svgX } = toSvgCoords(machineX, bbox.minY, bbox, paddingMm);
    lines.push({ x1: svgX, y1: 0, x2: svgX, y2: svgViewBox.height, key: `vx${machineX}` });
  }

  // Horizontal lines (constant Y in machine space)
  const startY = Math.floor(bbox.minY / step) * step;
  const endY = Math.ceil(bbox.maxY / step) * step;
  for (let machineY = startY; machineY <= endY; machineY += step) {
    const { svgY } = toSvgCoords(bbox.minX, machineY, bbox, paddingMm);
    lines.push({ x1: 0, y1: svgY, x2: svgViewBox.width, y2: svgY, key: `hy${machineY}` });
  }

  return (
    <g>
      {lines.map((l) => (
        <line key={l.key} className="kmGridLine" x1={l.x1} y1={l.y1} x2={l.x2} y2={l.y2} />
      ))}
    </g>
  );
}
