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
};

}  // namespace auto_typer
