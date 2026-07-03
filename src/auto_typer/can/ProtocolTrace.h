#pragma once

#include <Arduino.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include "../protocol/EmmV5ProtocolParser.h"
#include "CanFrame.h"

namespace auto_typer {

struct ProtocolTraceItem {
  uint32_t timeMs;
  const char* dir;
  uint32_t canId;
  bool extd;
  uint8_t dlc;
  uint8_t data[8];
  char dataHex[24];
  const char* parsed;
  uint8_t motorId;
  uint8_t command;
  uint8_t status;
  uint8_t packetIndex;
  bool hasMotionContext;
  char groupId[49];
  uint32_t seq;
  size_t blockIndex;
  char blockKind[24];
};

class ProtocolTrace {
 public:
  ProtocolTrace() : mutex_(nullptr), next_(0), count_(0), context_{} {
    for (uint8_t i = 0; i < kCapacity; ++i) {
      items_[i] = {};
    }
  }

  void setMotionContext(const char* groupId, uint32_t seq, size_t blockIndex, const char* blockKind) {
    if (!lock()) {
      return;
    }
    context_.active = true;
    copyText(context_.groupId, sizeof(context_.groupId), groupId);
    context_.seq = seq;
    context_.blockIndex = blockIndex;
    copyText(context_.blockKind, sizeof(context_.blockKind), blockKind);
    unlock();
  }

  void clearMotionContext() {
    if (!lock()) {
      return;
    }
    context_ = {};
    unlock();
  }

  void addTxQueued(const CanFrame& frame) {
    ProtocolTraceItem item = frameItem(frame, "tx_queued", "TxQueued");
    push(item);
  }

  void addTxSent(const CanFrame& frame) {
    ProtocolTraceItem item = frameItem(frame, "tx_sent", "TxSent");
    push(item);
  }

  void addTxRetry(const CanFrame& frame) {
    ProtocolTraceItem item = frameItem(frame, "tx_retry", "TxRetry");
    push(item);
  }

  void addTx(const CanFrame& frame) {
    addTxQueued(frame);
  }

  void addRx(const CanFrame& frame, const EmmV5Event& event) {
    ProtocolTraceItem item = frameItem(frame, "rx", event.errorCode != nullptr && event.errorCode[0] != '\0'
                                                        ? event.errorCode
                                                        : EmmV5ProtocolParser::kindText(event.kind));
    item.motorId = event.motorId;
    item.command = event.command;
    if (event.dlc > 1) {
      item.status = event.raw[1];
    }
    item.packetIndex = event.packetIndex;
    push(item);
  }

  size_t snapshot(ProtocolTraceItem* out, size_t maxItems) const {
    if (!lock()) {
      return 0;
    }
    const size_t n = count_ < maxItems ? count_ : maxItems;
    const size_t start = (next_ + kCapacity - count_) % kCapacity;
    for (size_t i = 0; i < n; ++i) {
      out[i] = items_[(start + i) % kCapacity];
    }
    unlock();
    return n;
  }

  static constexpr size_t capacity() {
    return kCapacity;
  }

 private:
  static constexpr size_t kCapacity = 128;

  struct MotionContext {
    bool active;
    char groupId[49];
    uint32_t seq;
    size_t blockIndex;
    char blockKind[24];
  };

  static ProtocolTraceItem frameItem(const CanFrame& frame, const char* dir, const char* parsed) {
    ProtocolTraceItem item{};
    item.timeMs = millis();
    item.dir = dir;
    item.canId = frame.identifier;
    item.extd = frame.extd;
    item.dlc = frame.data_length_code > 8 ? 8 : frame.data_length_code;
    item.parsed = parsed;
    item.motorId = frame.extd ? static_cast<uint8_t>((frame.identifier >> 8) & 0xFF) : 0;
    item.packetIndex = frame.extd ? static_cast<uint8_t>(frame.identifier & 0xFF) : 0;
    item.command = item.dlc > 0 ? frame.data[0] : 0;
    item.status = item.dlc > 1 ? frame.data[1] : 0;
    for (uint8_t i = 0; i < item.dlc; ++i) {
      item.data[i] = frame.data[i];
    }
    writeDataHex(item);
    return item;
  }

  static void writeDataHex(ProtocolTraceItem& item) {
    static const char kHex[] = "0123456789ABCDEF";
    size_t out = 0;
    for (uint8_t i = 0; i < item.dlc && out + 2 < sizeof(item.dataHex); ++i) {
      if (i > 0 && out + 1 < sizeof(item.dataHex)) {
        item.dataHex[out++] = ' ';
      }
      item.dataHex[out++] = kHex[(item.data[i] >> 4) & 0x0F];
      item.dataHex[out++] = kHex[item.data[i] & 0x0F];
    }
    item.dataHex[out] = '\0';
  }

  void push(const ProtocolTraceItem& item) {
    if (!lock()) {
      return;
    }
    ProtocolTraceItem stored = item;
    applyContextLocked(stored);
    items_[next_] = stored;
    next_ = (next_ + 1) % kCapacity;
    if (count_ < kCapacity) {
      ++count_;
    }
    unlock();
  }

  void applyContextLocked(ProtocolTraceItem& item) const {
    if (!context_.active) {
      return;
    }
    item.hasMotionContext = true;
    copyText(item.groupId, sizeof(item.groupId), context_.groupId);
    item.seq = context_.seq;
    item.blockIndex = context_.blockIndex;
    copyText(item.blockKind, sizeof(item.blockKind), context_.blockKind);
  }

  static void copyText(char* out, size_t outSize, const char* value) {
    if (outSize == 0) {
      return;
    }
    const char* source = value != nullptr ? value : "";
    size_t i = 0;
    for (; i + 1 < outSize && source[i] != '\0'; ++i) {
      out[i] = source[i];
    }
    out[i] = '\0';
  }

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
  ProtocolTraceItem items_[kCapacity];
  size_t next_;
  size_t count_;
  MotionContext context_;
};

}  // namespace auto_typer
