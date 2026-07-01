#pragma once

#include <Arduino.h>
#include "driver/twai.h"

namespace auto_typer {

using CanFrame = twai_message_t;

enum class CanBusFault : uint8_t {
  None,
  BusOff,
  TxFailed,
  RxQueueFull,
  BusError,
  ErrPassive,
};

struct CanBusDiagnostics {
  bool driverReady;
  bool motionReady;
  uint32_t lastAlerts;
  uint32_t txFailedCount;
  uint32_t txRetryCount;
  uint32_t commandQueueFullCount;
  uint32_t busErrorCount;
  uint32_t rxQueueFullCount;
  uint32_t errPassiveCount;
  uint32_t busOffCount;
  bool pendingFrameValid;
  uint32_t lastFaultAtMs;
  uint32_t lastAlertAtMs;
  const char* lastTxError;
  const char* lastCommandQueueError;
  bool recoverable;
  bool fatalFault;
  CanBusFault lastFault;
  const char* lastFaultCode;
  const char* lastFaultMessage;
};

}  // namespace auto_typer
