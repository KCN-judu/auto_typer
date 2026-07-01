import type { JobOverviewViewModel } from "../../domain/dashboardViewModel";

interface JobOverviewPanelProps {
  job: JobOverviewViewModel;
}

export function JobOverviewPanel({ job }: JobOverviewPanelProps) {
  if (!job.hasJob) {
    return (
      <div className="dashPanel">
        <div className="dashPanelHeader">
          <h3>当前任务</h3>
        </div>
        <p className="dashMuted">无活跃任务</p>
      </div>
    );
  }

  return (
    <div className="dashPanel">
      <div className="dashPanelHeader">
        <h3>当前任务</h3>
        <span className={`dashJobBadge state-${job.state}`}>{job.state.toUpperCase()}</span>
      </div>
      <div className="dashJobGrid">
        <div className="dashJobField">
          <span className="dashJobLabel">ID</span>
          <span className="mono">{job.jobId}</span>
        </div>
        <div className="dashJobField">
          <span className="dashJobLabel">进度</span>
          <span className="mono">{job.currentStep} / {job.totalSteps}</span>
        </div>
        <div className="dashJobField">
          <span className="dashJobLabel">Block</span>
          <span className="mono">{job.currentBlock} / {job.totalBlocks}</span>
        </div>
        <div className="dashJobField">
          <span className="dashJobLabel">坐标</span>
          <span className="mono">{job.position} mm</span>
        </div>
      </div>
      {/* Progress bar */}
      <div className="dashJobProgress">
        <div className="dashJobProgressTrack">
          <div className="dashJobProgressFill" style={{ width: `${job.progressPercent}%` }} />
        </div>
        <span className="dashJobPercent mono">{job.progressPercent}%</span>
      </div>
      {job.message && <p className="dashJobMessage">{job.message}</p>}
    </div>
  );
}
