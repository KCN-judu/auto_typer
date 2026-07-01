import type { StatusTone } from "../../domain/dashboardViewModel";

interface BarMeterProps {
  value: number | null;
  min: number;
  max: number;
  label: string;
  unit: string;
  status?: StatusTone;
}

export function BarMeter({ value, min, max, label, unit, status = "unknown" }: BarMeterProps) {
  if (value === null || value === undefined) {
    return (
      <div className="dashBar">
        <div className="dashBarHeader">
          <span className="dashBarLabel">{label}</span>
          <span className="dashBarValue mono">N/A</span>
        </div>
        <div className="dashBarTrack">
          <div className="dashBarFill" style={{ width: "0%" }} />
        </div>
      </div>
    );
  }

  const clamped = Math.max(min, Math.min(max, value));
  const percent = ((clamped - min) / (max - min || 1)) * 100;
  const toneColor = status === "ok" ? "var(--dash-ok)" : status === "warning" ? "var(--dash-warn)" : status === "fault" ? "var(--dash-fault)" : "var(--dash-accent)";

  return (
    <div className="dashBar">
      <div className="dashBarHeader">
        <span className="dashBarLabel">{label}</span>
        <span className="dashBarValue mono">{value % 1 === 0 ? value : value.toFixed(1)} {unit}</span>
      </div>
      <div className="dashBarTrack">
        <div className="dashBarFill" style={{ width: `${percent}%`, background: toneColor }} />
      </div>
    </div>
  );
}
