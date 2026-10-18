#pragma once

#include <Arduino.h>
#include "link_protocol.hpp"

namespace openscope {

struct ReceivedFrame {
  link_protocol::Kind kind;
  link_protocol::Service service;
  uint8_t opcode;
  uint16_t transactionId;
  const uint8_t* payload;
  size_t payloadSize;
};

using FrameHandler = void (*)(const ReceivedFrame&, void*);

class Link {
  friend class MeterBridge;
 public:
  static Link& instance();
  void begin(FrameHandler handler, void* context);
  void loop();
  bool send(link_protocol::Kind kind, link_protocol::Service service, uint8_t opcode,
            const void* payload, size_t payloadSize, uint16_t transactionId = 0,
            uint8_t flags = link_protocol::PayloadRaw);
  bool connected() const;
  uint16_t sessionId() const { return sessionId_; }
  uint32_t rxBytes() const { return rxBytes_; }
  uint32_t txBytes() const { return txBytes_; }
  uint32_t frameAttempts() const { return frameAttempts_; }
  uint32_t validFrames() const { return validFrames_; }
  uint32_t helloCount() const { return helloCount_; }
  bool transmitQuiesced() const { return transmitQuiesced_; }

 private:
  Link() = default;
  void handleDecoded(const uint8_t* decoded, size_t length);
  void sendHello();
  static uint32_t crc32c(const uint8_t* data, size_t size);
  static size_t cobsEncode(const uint8_t* input, size_t length, uint8_t* output, size_t capacity);
  static size_t cobsDecode(const uint8_t* input, size_t length, uint8_t* output, size_t capacity);

  FrameHandler handler_ = nullptr;
  void* handlerContext_ = nullptr;
  uint16_t sessionId_ = 0;
  uint16_t txSequence_ = 0;
  uint16_t txTransaction_ = 0;
  uint32_t bootNonce_ = 0;
  uint32_t lastRxMs_ = 0;
  uint32_t lastHeartbeatMs_ = 0;
  uint32_t lastByteMs_ = 0;
  uint32_t quiescedSinceMs_ = 0;
  uint16_t quiesceLeaseMs_ = 1000;
  uint32_t rxBytes_ = 0;
  uint32_t txBytes_ = 0;
  uint32_t frameAttempts_ = 0;
  uint32_t validFrames_ = 0;
  uint32_t helloCount_ = 0;
  bool transmitQuiesced_ = false;
};

}  // namespace openscope
