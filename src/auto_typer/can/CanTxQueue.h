#pragma once

#include <Arduino.h>
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"

#include "CanBus.h"
#include "ProtocolTrace.h"

namespace auto_typer {

class CanTxQueue {
 public:
  explicit CanTxQueue(CanBus& bus, ProtocolTrace* trace = nullptr)
      : bus_(bus),
        trace_(trace),
        queue_(nullptr),
        mutex_(xSemaphoreCreateMutex()),
        pendingFrame_{},
        pendingValid_(false) {}

  bool begin(size_t capacity = 32) {
    if (queue_ != nullptr) {
      return true;
    }
    queue_ = xQueueCreate(capacity, sizeof(QueuedFrame));
    return queue_ != nullptr;
  }

  bool enqueue(const CanFrame& frame, bool highPriority = false) {
    return enqueueBatch(&frame, 1, highPriority);
  }

  size_t availableForWrite() {
    if (queue_ == nullptr && !begin()) {
      return 0;
    }
    return uxQueueSpacesAvailable(queue_);
  }

  bool enqueueBatch(const CanFrame* frames, size_t count, bool highPriority = false) {
    if (count == 0) {
      return true;
    }
    if (frames == nullptr || (queue_ == nullptr && !begin())) {
      return false;
    }
    if (!lock()) {
      return false;
    }
    if (uxQueueSpacesAvailable(queue_) < count) {
      unlock();
      bus_.recordCommandQueueFull();
      return false;
    }
    if (highPriority) {
      for (size_t i = count; i > 0; --i) {
        QueuedFrame item{frames[i - 1], highPriority};
        xQueueSendToFront(queue_, &item, 0);
      }
    } else {
      for (size_t i = 0; i < count; ++i) {
        QueuedFrame item{frames[i], highPriority};
        xQueueSendToBack(queue_, &item, 0);
      }
    }
    unlock();
    return true;
  }

  void tick(size_t maxFrames = 4) {
    if (queue_ == nullptr) {
      return;
    }
    size_t sent = 0;
    while (sent < maxFrames) {
      if (bus_.hasFatalFault() && pendingValid_ && !pendingFrame_.highPriority) {
        pendingValid_ = false;
        bus_.setPendingFrameValid(false);
      }
      if (!pendingValid_ && !loadNextPendingFrame()) {
        return;
      }
      if (!bus_.transmit(pendingFrame_.frame)) {
        bus_.recordTxRetry();
        if (trace_ != nullptr) {
          trace_->addTxRetry(pendingFrame_.frame);
        }
        return;
      }
      if (trace_ != nullptr) {
        trace_->addTxSent(pendingFrame_.frame);
      }
      pendingValid_ = false;
      bus_.setPendingFrameValid(false);
      ++sent;
    }
  }

  void clear() {
    if (queue_ != nullptr) {
      xQueueReset(queue_);
    }
    pendingValid_ = false;
    bus_.setPendingFrameValid(false);
  }

 private:
  struct QueuedFrame {
    CanFrame frame;
    bool highPriority;
  };

  bool loadNextPendingFrame() {
    QueuedFrame item{};
    while (xQueueReceive(queue_, &item, 0) == pdTRUE) {
      if (bus_.hasFatalFault() && !item.highPriority) {
        continue;
      }
      pendingFrame_ = item;
      pendingValid_ = true;
      bus_.setPendingFrameValid(true);
      return true;
    }
    return false;
  }

  bool lock() const {
    return mutex_ == nullptr || xSemaphoreTake(mutex_, pdMS_TO_TICKS(5)) == pdTRUE;
  }

  void unlock() const {
    if (mutex_ != nullptr) {
      xSemaphoreGive(mutex_);
    }
  }

  CanBus& bus_;
  ProtocolTrace* trace_;
  QueueHandle_t queue_;
  SemaphoreHandle_t mutex_;
  QueuedFrame pendingFrame_;
  bool pendingValid_;
};

}  // namespace auto_typer
