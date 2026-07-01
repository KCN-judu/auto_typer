#pragma once

#include <Arduino.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include "../protocol/EmmV5ProtocolParser.h"

namespace auto_typer {

class EmmV5EventStore {
 public:
  EmmV5EventStore() : mutex_(xSemaphoreCreateMutex()), next_(0), count_(0) {
    for (uint8_t i = 0; i < kCapacity; ++i) {
      events_[i] = {};
    }
  }

  void push(const EmmV5Event& event) {
    if (!lock()) {
      return;
    }
    events_[next_] = event;
    next_ = (next_ + 1) % kCapacity;
    if (count_ < kCapacity) {
      ++count_;
    }
    unlock();
  }

  size_t snapshot(EmmV5Event* out, size_t maxEvents) const {
    if (!lock()) {
      return 0;
    }
    const size_t n = count_ < maxEvents ? count_ : maxEvents;
    const size_t start = (next_ + kCapacity - count_) % kCapacity;
    for (size_t i = 0; i < n; ++i) {
      out[i] = events_[(start + i) % kCapacity];
    }
    unlock();
    return n;
  }

 private:
  static constexpr size_t kCapacity = 64;

  bool lock() const {
    return mutex_ == nullptr || xSemaphoreTake(mutex_, pdMS_TO_TICKS(5)) == pdTRUE;
  }

  void unlock() const {
    if (mutex_ != nullptr) {
      xSemaphoreGive(mutex_);
    }
  }

  mutable SemaphoreHandle_t mutex_;
  EmmV5Event events_[kCapacity];
  size_t next_;
  size_t count_;
};

}  // namespace auto_typer
