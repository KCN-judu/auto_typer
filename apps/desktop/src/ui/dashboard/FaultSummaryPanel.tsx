import type { FaultSummaryViewModel } from "../../domain/dashboardViewModel";
import { StatusDot } from "./StatusDot";

interface FaultSummaryPanelProps {
  fault: FaultSummaryViewModel;
}

export function FaultSummaryPanel({ fault }: FaultSummaryPanelProps) {
  if (!fault.hasFault) {
    return (
      <div className="dashPanel">
        <div className="dashPanelHeader">
          <h3>故障摘要</h3>
          <StatusDot tone="ok" size={8} />
        </div>
        <p className="dashMuted">无故障</p>
      </div>
    );
  }

  return (
    <div className="dashPanel dashPanelFault">
      <div className="dashPanelHeader">
        <h3>故障摘要</h3>
        <StatusDot tone="fault" size={8} pulse />
      </div>
      {fault.code && (
        <div className="dashFaultMain">
          <span className="dashFaultCode mono">{fault.code}</span>
          <span>{fault.message}</span>
          {fault.recoverable && <span className="dashFaultRecoverable">可恢复</span>}
        </div>
      )}
      {fault.motorFaults.length > 0 && (
        <div className="dashFaultList">
          {fault.motorFaults.map((f) => (
            <div key={f} className="dashFaultItem motor">{f}</div>
          ))}
        </div>
      )}
      {fault.canFault && <div className="dashFaultItem can">{fault.canFault}</div>}
    </div>
  );
}
