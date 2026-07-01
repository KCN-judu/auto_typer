import type { StatusTone } from "../../domain/dashboardViewModel";

interface GaugeMeterProps {
  value: number | null;
  min: number;
  max: number;
  label: string;
  unit: string;
  status?: StatusTone;
}

const GAUGE_RADIUS = 36;
const STROKE_WIDTH = 6;
const SWEEP_ANGLE = 240; // degrees of the arc
const START_ANGLE = 150; // start offset from top (degrees)

export function GaugeMeter({ value, min, max, label, unit, status = "unknown" }: GaugeMeterProps) {
  const size = (GAUGE_RADIUS + STROKE_WIDTH) * 2;
  const cx = size / 2;
  const cy = size / 2;

  // Convert degrees to radians for SVG arc
  const startRad = (START_ANGLE * Math.PI) / 180;
  const endRad = ((START_ANGLE + SWEEP_ANGLE) * Math.PI) / 180;

  // Background arc path
  const bgPath = describeArc(cx, cy, GAUGE_RADIUS, startRad, endRad);

  // Value arc
  let valuePath = "";
  let displayValue = "N/A";
  if (value !== null && value !== undefined) {
    const clamped = Math.max(min, Math.min(max, value));
    const ratio = (clamped - min) / (max - min || 1);
    const valueEndRad = startRad + ratio * ((SWEEP_ANGLE * Math.PI) / 180);
    valuePath = describeArc(cx, cy, GAUGE_RADIUS, startRad, valueEndRad);
    displayValue = value % 1 === 0 ? `${value}` : value.toFixed(1);
  }

  const toneColor = status === "ok" ? "var(--dash-ok)" : status === "warning" ? "var(--dash-warn)" : status === "fault" ? "var(--dash-fault)" : "var(--dash-accent)";

  return (
    <div className="dashGauge">
      <svg width={size} height={size} viewBox={`0 0 ${size} ${size}`} className="dashGaugeSvg">
        {/* Background track */}
        <path d={bgPath} fill="none" stroke="var(--dash-track)" strokeWidth={STROKE_WIDTH} strokeLinecap="round" />
        {/* Value arc */}
        {valuePath && (
          <path d={valuePath} fill="none" stroke={toneColor} strokeWidth={STROKE_WIDTH} strokeLinecap="round" />
        )}
      </svg>
      <div className="dashGaugeCenter">
        <span className="dashGaugeValue">{displayValue}</span>
        <span className="dashGaugeUnit">{unit}</span>
      </div>
      <span className="dashGaugeLabel">{label}</span>
    </div>
  );
}

function describeArc(cx: number, cy: number, r: number, startRad: number, endRad: number): string {
  const x1 = cx + r * Math.cos(startRad);
  const y1 = cy + r * Math.sin(startRad);
  const x2 = cx + r * Math.cos(endRad);
  const y2 = cy + r * Math.sin(endRad);
  const sweep = endRad - startRad;
  const largeArc = sweep > Math.PI ? 1 : 0;
  return `M ${x1} ${y1} A ${r} ${r} 0 ${largeArc} 1 ${x2} ${y2}`;
}
