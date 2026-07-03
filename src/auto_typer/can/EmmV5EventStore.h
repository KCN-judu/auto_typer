#pragma once

#include <Arduino.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include "../protocol/EmmV5ProtocolParser.h"

namespace auto_typer {

struct ProtocolDiagnostics {
  uint32_t unknownFrameCount;
  uint32_t invalidFrameCount;
  uint32_t lastEventAtMs;
  uint32_t lastInvalidAtMs;
  const char* lastInvalidError;
  EmmV5EventKind lastEventKind;
};

class EmmV5EventStore {
 public:
  EmmV5EventStore()
      : mutex_(nullptr),
        next_(0),
        count_(0),
        unknownFrameCount_(0),
        invalidFrameCount_(0),
        lastEventAtMs_(0),
        lastInvalidAtMs_(0),
        lastInvalidError_(""),
        lastEventKind_(EmmV5EventKind::None) {
    for (uint8_t i = 0; i < kCapacity; ++i) {
      events_[i] = {};
    }
  }

  void push(const EmmV5Event& event) {
    if (!lock()) {
      return;
    }
    lastEventAtMs_ = event.timeMs;
    lastEventKind_ = event.kind;
    if (event.kind == EmmV5EventKind::UnknownFrame) {
      ++unknownFrameCount_;
    } else if (event.kind == EmmV5EventKind::InvalidFrame) {
      ++invalidFrameCount_;
      lastInvalidAtMs_ = event.timeMs;
      lastInvalidError_ = event.errorCode;
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

  ProtocolDiagnostics diagnostics() const {
    if (!lock()) {
      return {};
    }
    ProtocolDiagnostics snapshot{};
    snapshot.unknownFrameCount = unknownFrameCount_;
    snapshot.invalidFrameCount = invalidFrameCount_;
    snapshot.lastEventAtMs = lastEventAtMs_;
    snapshot.lastInvalidAtMs = lastInvalidAtMs_;
    snapshot.lastInvalidError = lastInvalidError_;
    snapshot.lastEventKind = lastEventKind_;
    unlock();
    return snapshot;
  }

 private:
  static constexpr size_t kCapacity = 64;

  bool lock() const {
    return ensureMutex() && xSemaphoreTake(mutex_, pdMS_TO_TICKS(5)) == pdTRUE;
  }

  void unlock() const {
    if (mutex_ != nullptr) {
      xSemaphoreGive(mutex_);
    }
  }

  bool ensureMutex() const {
    if (mutex_ == nullptr) {
      mutex_ = xSemaphoreCreateMutex();
    }
    return mutex_ != nullptr;
  }

  mutable SemaphoreHandle_t mutex_;
  EmmV5Event events_[kCapacity];
  size_t next_;
  size_t count_;
  uint32_t unknownFrameCount_;
  uint32_t invalidFrameCount_;
  uint32_t lastEventAtMs_;
  uint32_t lastInvalidAtMs_;
  const char* lastInvalidError_;
  EmmV5EventKind lastEventKind_;
};

}  // namespace auto_typer
