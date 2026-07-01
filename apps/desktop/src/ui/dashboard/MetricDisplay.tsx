interface MetricDisplayProps {
  label: string;
  value: string | number | null;
  unit?: string;
  mono?: boolean;
}

export function MetricDisplay({ label, value, unit, mono = true }: MetricDisplayProps) {
  const display = value === null || value === undefined ? "N/A" : `${value}`;
  return (
    <div className="dashMetric">
      <span className="dashMetricLabel">{label}</span>
      <span className={`dashMetricValue ${mono ? "mono" : ""}`}>
        {display}
        {value !== null && value !== undefined && unit && <span className="dashMetricUnit">{unit}</span>}
      </span>
    </div>
  );
}
