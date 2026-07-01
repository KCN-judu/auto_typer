import type { SummaryItem } from "../../domain/dashboardViewModel";
import { StatusDot } from "./StatusDot";

interface SummaryModuleProps {
  item: SummaryItem;
}

export function SummaryModule({ item }: SummaryModuleProps) {
  return (
    <div className="dashSummaryModule">
      <div className="dashSummaryHeader">
        <StatusDot tone={item.tone} size={8} />
        <span className="dashSummaryLabel">{item.label}</span>
      </div>
      <span className="dashSummaryValue mono">{item.value}</span>
    </div>
  );
}
