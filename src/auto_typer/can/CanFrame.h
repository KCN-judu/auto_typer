#pragma once

#include <Arduino.h>

namespace auto_typer {

struct CanFrame {
  uint32_t identifier;
  uint8_t data_length_code;
  uint8_t data[8];
  bool extd;
  bool rtr;
};

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
