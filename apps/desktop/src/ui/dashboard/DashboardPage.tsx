import { useMemo } from "react";
import type { DeviceStatus } from "../../../../../shared/protocol/auto-typer-protocol";
import {
  buildCanOverview,
  buildDashboardSummary,
  buildFaultSummary,
  buildJobOverview,
  buildMotorCards,
} from "../../domain/dashboardViewModel";
import { CanOverviewPanel } from "./CanOverviewPanel";
import { FaultSummaryPanel } from "./FaultSummaryPanel";
import { JobOverviewPanel } from "./JobOverviewPanel";
import { MotorStatusCard } from "./MotorStatusCard";
import { SummaryModule } from "./SummaryModule";

interface DashboardPageProps {
  status: DeviceStatus;
  connectionState: string;
}

export function DashboardPage({ status, connectionState }: DashboardPageProps) {
  const summary = useMemo(() => buildDashboardSummary(status, connectionState), [status, connectionState]);
  const motorCards = useMemo(() => buildMotorCards(status), [status]);
  const canOverview = useMemo(() => buildCanOverview(status.canDiagnostics), [status.canDiagnostics]);
  const jobOverview = useMemo(() => buildJobOverview(status.currentJob), [status.currentJob]);
  const faultSummary = useMemo(() => buildFaultSummary(status), [status]);

  return (
    <section className="dashPage">
      {/* ── Top: Summary Modules ── */}
      <div className="dashSummaryGrid">
        <SummaryModule item={summary.connection} />
        <SummaryModule item={summary.mode} />
        <SummaryModule item={summary.health} />
        <SummaryModule item={summary.jobState} />
        <SummaryModule item={summary.coordinates} />
        <SummaryModule item={summary.groupProgress} />
        <SummaryModule item={summary.canState} />
      </div>

      {/* ── Middle: Motor Telemetry ── */}
      <h2 className="dashSectionTitle">电机遥测</h2>
      <div className="dashMotorGrid">
        {motorCards.map((motor) => (
          <MotorStatusCard key={motor.id} motor={motor} />
        ))}
      </div>

      {/* ── Bottom: Panels ── */}
      <div className="dashBottomGrid">
        <CanOverviewPanel can={canOverview} />
        <JobOverviewPanel job={jobOverview} />
        <FaultSummaryPanel fault={faultSummary} />
      </div>
    </section>
  );
}
