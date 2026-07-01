import type { StatusTone } from "../../domain/dashboardViewModel";

interface StatusDotProps {
  tone: StatusTone;
  size?: number;
  /** Pulsate when warning/fault */
  pulse?: boolean;
}

const toneColors: Record<StatusTone, string> = {
  ok: "var(--dash-ok)",
  warning: "var(--dash-warn)",
  fault: "var(--dash-fault)",
  unknown: "var(--dash-muted)",
};

export function StatusDot({ tone, size = 10, pulse = false }: StatusDotProps) {
  const color = toneColors[tone];
  return (
    <span
      className={`dashStatusDot ${pulse && (tone === "fault" || tone === "warning") ? "pulse" : ""}`}
      style={{ width: size, height: size, background: color }}
      aria-label={tone}
    />
  );
}
