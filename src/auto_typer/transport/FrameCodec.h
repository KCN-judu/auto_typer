#pragma once

#include <Arduino.h>
#include <ArduinoJson.h>

namespace auto_typer {

static constexpr uint8_t kFrameMagic0 = 0x41;
static constexpr uint8_t kFrameMagic1 = 0x54;
static constexpr uint8_t kFrameVersion = 1;
static constexpr uint8_t kFrameHeaderLen = 16;
static constexpr size_t kMaxPayloadBytes = 4096;

enum class DesktopFrameType : uint16_t {
  Hello = 1,
  HelloAck = 2,
  Command = 10,
  Ack = 11,
  Done = 12,
  Fault = 13,
  Telemetry = 20,
  Snapshot = 21,
  Ping = 30,
  Pong = 31,
};

struct DecodedFrame {
  DesktopFrameType type;
  uint32_t seq;
  const uint8_t* payload;
  size_t payloadLen;
};

class FrameCodec {
 public:
  enum class ReadResult : uint8_t {
    NeedMore,
    FrameReady,
    ProtocolError,
  };

  FrameCodec() : bufferLen_(0), frameReady_(false), protocolError_(""), readyType_(DesktopFrameType::Fault), readySeq_(0), readyPayloadLen_(0) {}

  void reset() {
    bufferLen_ = 0;
    frameReady_ = false;
    protocolError_ = "";
    readyPayloadLen_ = 0;
  }

  const char* protocolError() const {
    return protocolError_;
  }

  ReadResult push(uint8_t byte) {
    if (bufferLen_ >= sizeof(buffer_)) {
      protocolError_ = "frame_buffer_overflow";
      return ReadResult::ProtocolError;
    }
    buffer_[bufferLen_] = byte;
    ++bufferLen_;
    return parseBuffered();
  }

  bool takeFrame(DecodedFrame& frame) {
    if (!frameReady_) {
      return false;
    }
    frame.type = readyType_;
    frame.seq = readySeq_;
    frame.payload = buffer_ + kFrameHeaderLen;
    frame.payloadLen = readyPayloadLen_;
    return true;
  }

  void consumeFrame() {
    if (!frameReady_) {
      return;
    }
    const size_t frameLen = kFrameHeaderLen + readyPayloadLen_;
    const size_t remaining = bufferLen_ > frameLen ? bufferLen_ - frameLen : 0;
    if (remaining > 0) {
      memmove(buffer_, buffer_ + frameLen, remaining);
    }
    bufferLen_ = remaining;
    frameReady_ = false;
    readyPayloadLen_ = 0;
    parseBuffered();
  }

  template <typename TClient, typename TDoc>
  static bool writeJsonFrame(TClient& client, DesktopFrameType type, uint32_t seq, TDoc& doc) {
    if (!client || !client.connected()) {
      return false;
    }
    char payload[kMaxPayloadBytes + 1];
    const size_t payloadLen = serializeJson(doc, payload, sizeof(payload));
    if (payloadLen == 0 || payloadLen > kMaxPayloadBytes) {
      return false;
    }
    uint8_t header[kFrameHeaderLen] = {};
    header[0] = kFrameMagic0;
    header[1] = kFrameMagic1;
    header[2] = kFrameVersion;
    header[3] = kFrameHeaderLen;
    writeLe16(header + 4, static_cast<uint16_t>(type));
    writeLe16(header + 6, 0);
    writeLe32(header + 8, seq);
    writeLe32(header + 12, static_cast<uint32_t>(payloadLen));
    return client.write(header, sizeof(header)) == sizeof(header) &&
           client.write(reinterpret_cast<const uint8_t*>(payload), payloadLen) == payloadLen;
  }

 private:
  ReadResult parseBuffered() {
    if (frameReady_) {
      return ReadResult::FrameReady;
    }
    if (bufferLen_ < kFrameHeaderLen) {
      return ReadResult::NeedMore;
    }
    if (buffer_[0] != kFrameMagic0 || buffer_[1] != kFrameMagic1) {
      protocolError_ = "bad_magic";
      return ReadResult::ProtocolError;
    }
    if (buffer_[2] != kFrameVersion) {
      protocolError_ = "bad_version";
      return ReadResult::ProtocolError;
    }
    if (buffer_[3] != kFrameHeaderLen) {
      protocolError_ = "bad_header_len";
      return ReadResult::ProtocolError;
    }
    const uint32_t payloadLen = readLe32(buffer_ + 12);
    if (payloadLen > kMaxPayloadBytes) {
      protocolError_ = "payload_too_large";
      return ReadResult::ProtocolError;
    }
    const size_t frameLen = kFrameHeaderLen + payloadLen;
    if (bufferLen_ < frameLen) {
      return ReadResult::NeedMore;
    }
    readyType_ = static_cast<DesktopFrameType>(readLe16(buffer_ + 4));
    readySeq_ = readLe32(buffer_ + 8);
    readyPayloadLen_ = payloadLen;
    frameReady_ = true;
    return ReadResult::FrameReady;
  }

  static uint16_t readLe16(const uint8_t* bytes) {
    return static_cast<uint16_t>(bytes[0]) | (static_cast<uint16_t>(bytes[1]) << 8);
  }

  static uint32_t readLe32(const uint8_t* bytes) {
    return static_cast<uint32_t>(bytes[0]) | (static_cast<uint32_t>(bytes[1]) << 8) |
           (static_cast<uint32_t>(bytes[2]) << 16) | (static_cast<uint32_t>(bytes[3]) << 24);
  }

  static void writeLe16(uint8_t* out, uint16_t value) {
    out[0] = static_cast<uint8_t>(value & 0xff);
    out[1] = static_cast<uint8_t>((value >> 8) & 0xff);
  }

  static void writeLe32(uint8_t* out, uint32_t value) {
    out[0] = static_cast<uint8_t>(value & 0xff);
    out[1] = static_cast<uint8_t>((value >> 8) & 0xff);
    out[2] = static_cast<uint8_t>((value >> 16) & 0xff);
    out[3] = static_cast<uint8_t>((value >> 24) & 0xff);
  }

  uint8_t buffer_[kFrameHeaderLen + kMaxPayloadBytes];
  size_t bufferLen_;
  bool frameReady_;
  const char* protocolError_;
  DesktopFrameType readyType_;
  uint32_t readySeq_;
  size_t readyPayloadLen_;
};

}  // namespace auto_typer
