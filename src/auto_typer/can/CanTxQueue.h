#pragma once

#include <Arduino.h>
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

#include "CanBus.h"

namespace auto_typer {

class CanTxQueue {
 public:
  explicit CanTxQueue(CanBus& bus) : bus_(bus), queue_(nullptr) {}

  bool begin(size_t capacity = 32) {
    if (queue_ != nullptr) {
      return true;
    }
    queue_ = xQueueCreate(capacity, sizeof(QueuedFrame));
    return queue_ != nullptr;
  }

  bool enqueue(const CanFrame& frame, bool highPriority = false) {
    if (queue_ == nullptr && !begin()) {
      return false;
    }
    QueuedFrame item{frame, highPriority};
    if (highPriority) {
      return xQueueSendToFront(queue_, &item, 0) == pdTRUE;
    }
    return xQueueSendToBack(queue_, &item, 0) == pdTRUE;
  }

  void tick(size_t maxFrames = 4) {
    if (queue_ == nullptr) {
      return;
    }
    QueuedFrame item{};
    size_t sent = 0;
    while (sent < maxFrames && xQueueReceive(queue_, &item, 0) == pdTRUE) {
      if (!bus_.transmit(item.frame)) {
        return;
      }
      ++sent;
    }
  }

  void clear() {
    if (queue_ != nullptr) {
      xQueueReset(queue_);
    }
  }

 private:
  struct QueuedFrame {
    CanFrame frame;
    bool highPriority;
  };

  CanBus& bus_;
  QueueHandle_t queue_;
};

}  // namespace auto_typer
