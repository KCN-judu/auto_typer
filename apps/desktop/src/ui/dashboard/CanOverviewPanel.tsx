import type { CanOverviewViewModel } from "../../domain/dashboardViewModel";
import { StatusDot } from "./StatusDot";

interface CanOverviewPanelProps {
  can: CanOverviewViewModel;
}

export function CanOverviewPanel({ can }: CanOverviewPanelProps) {
  if (!can.available) {
    return (
      <div className="dashPanel">
        <div className="dashPanelHeader">
          <h3>CAN 总线</h3>
          <StatusDot tone="unknown" size={8} />
        </div>
        <p className="dashMuted">CAN 诊断不可用</p>
      </div>
    );
  }

  return (
    <div className="dashPanel">
      <div className="dashPanelHeader">
        <h3>CAN 总线</h3>
        <StatusDot tone={can.tone} size={8} pulse={can.fatalFault} />
      </div>
      <div className="dashCanGrid">
        <CanRow label="Driver" ready={can.driverReady} />
        <CanRow label="Motion" ready={can.motionReady} />
        <CanRow label="Fatal" ready={!can.fatalFault} invertLabel />
        <CanRow label="Pending" ready={can.pendingFrame} />
      </div>
      <div className="dashCanCounters">
        <CounterChip label="TX Fail" value={can.txFailed} warn={can.txFailed > 0} />
        <CounterChip label="Retry" value={can.txRetry} warn={can.txRetry > 10} />
        <CounterChip label="Bus Err" value={can.busErrors} warn={can.busErrors > 0} />
        <CounterChip label="Q Full" value={can.queueFull} warn={can.queueFull > 0} />
        <CounterChip label="RX Full" value={can.rxQueueFull} warn={can.rxQueueFull > 0} />
      </div>
      {can.lastFault && <div className="dashCanFault">{can.lastFault}</div>}
      {can.lastTxError && <div className="dashCanWarn">{can.lastTxError}</div>}
    </div>
  );
}

function CanRow({ label, ready, invertLabel }: { label: string; ready: boolean; invertLabel?: boolean }) {
  const tone = invertLabel ? (ready ? "ok" : "fault") : (ready ? "ok" : "warning");
  return (
    <div className="dashCanRow">
      <StatusDot tone={tone} size={7} />
      <span>{label}</span>
      <span className="mono">{ready ? (invertLabel ? "NO" : "READY") : (invertLabel ? "YES" : "WAIT")}</span>
    </div>
  );
}

function CounterChip({ label, value, warn }: { label: string; value: number; warn: boolean }) {
  return (
    <div className={`dashCounterChip ${warn ? "warn" : ""}`}>
      <span className="dashCounterLabel">{label}</span>
      <span className="dashCounterValue mono">{value}</span>
    </div>
  );
}
