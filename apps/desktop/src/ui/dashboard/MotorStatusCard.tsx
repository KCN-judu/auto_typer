import type { MotorCardViewModel } from "../../domain/dashboardViewModel";
import { formatAge } from "../../domain/dashboardViewModel";
import { BarMeter } from "./BarMeter";
import { GaugeMeter } from "./GaugeMeter";
import { MetricDisplay } from "./MetricDisplay";
import { StatusDot } from "./StatusDot";

interface MotorStatusCardProps {
  motor: MotorCardViewModel;
}

export function MotorStatusCard({ motor }: MotorStatusCardProps) {
  const rpmStatus = motor.hasFault ? "fault" : motor.tone;

  return (
    <div className={`dashMotorCard ${motor.hasFault ? "hasFault" : ""}`}>
      {/* Header */}
      <div className="dashMotorHeader">
        <div className="dashMotorId">
          <StatusDot tone={motor.tone} size={10} pulse={motor.hasFault} />
          <span className="dashMotorLabel">{motor.label}</span>
          <span className="dashMotorRole">{motor.role}</span>
        </div>
        <span className={`dashMotorBadge tone-${motor.tone}`}>{motor.readiness}</span>
      </div>

      {/* Gauge + Metrics row */}
      <div className="dashMotorBody">
        <GaugeMeter
          value={motor.rpm}
          min={0}
          max={3000}
          label="RPM"
          unit="rpm"
          status={rpmStatus}
        />
        <div className="dashMotorMetrics">
          <MetricDisplay label="Pulse" value={motor.inputPulseSteps} unit="pulses" />
          <MetricDisplay label="Angle" value={motor.angleDeg !== null ? motor.angleDeg.toFixed(1) : null} unit="deg" />
          <MetricDisplay label="Updated" value={formatAge(motor.lastUpdateMs)} />
        </div>
      </div>

      {/* Bar meters */}
      <div className="dashMotorBars">
        <BarMeter value={motor.current} min={0} max={5} label="Current" unit="A" status={motor.current !== null && motor.current > 4 ? "warning" : "ok"} />
        <BarMeter value={motor.temperature} min={0} max={100} label="Temp" unit="°C" status={motor.temperature !== null && motor.temperature > 70 ? "warning" : "ok"} />
      </div>

      {/* Footer: event / fault */}
      {(motor.lastEvent || motor.faultText) && (
        <div className="dashMotorFooter">
          {motor.faultText && <span className="dashMotorFault">{motor.faultText}</span>}
          {motor.lastEvent && !motor.faultText && <span className="dashMotorEvent">{motor.lastEvent}</span>}
        </div>
      )}
    </div>
  );
}
