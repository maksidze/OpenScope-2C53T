#include "link.hpp"

#include <cstring>
#include "config.hpp"
#include "meter_bridge.hpp"

namespace openscope {
namespace {
void putU16(uint8_t* p, uint16_t value) {
  p[0] = static_cast<uint8_t>(value);
  p[1] = static_cast<uint8_t>(value >> 8);
}
void putU32(uint8_t* p, uint32_t value) {
  p[0] = static_cast<uint8_t>(value);
  p[1] = static_cast<uint8_t>(value >> 8);
  p[2] = static_cast<uint8_t>(value >> 16);
  p[3] = static_cast<uint8_t>(value >> 24);
}
uint16_t randomNonzero16() {
  uint16_t value = static_cast<uint16_t>(random(1, 65536));
  return value ? value : 1;
}
}  // namespace

Link& Link::instance() {
  static Link link;
  return link;
}

void Link::begin(FrameHandler handler, void* context) {
  handler_ = handler;
  handlerContext_ = context;
  // The independent bridge task owns physical UARTs even when an app blocks.
  bootNonce_ = static_cast<uint32_t>(random(1, 0x7fffffff));
  lastHeartbeatMs_ = millis() - config::kHeartbeatMs;
}

bool Link::connected() const {
  return sessionId_ != 0 && static_cast<uint32_t>(millis() - lastRxMs_) < config::kLinkTimeoutMs;
}

void Link::loop() {
  MeterBridge::Packet packet;
  while (MeterBridge::instance().receiveControl(packet)) {
    rxBytes_+=packet.wireLength;
    handleDecoded(packet.data,packet.length);
  }

  // RESUME normally arrives a few milliseconds after the LCD transaction.
  // Do not let an AT32 reset or a damaged RESUME frame leave the coprocessor
  // silent forever: after one second, return to HELLO-based discovery.
  if (transmitQuiesced_ &&
      static_cast<uint32_t>(millis() - quiescedSinceMs_) >= quiesceLeaseMs_) {
    transmitQuiesced_ = false;
    sessionId_ = 0;
    lastHeartbeatMs_ = millis() - config::kHeartbeatMs;
  }

  if (!transmitQuiesced_ &&
      static_cast<uint32_t>(millis() - lastHeartbeatMs_) >= config::kHeartbeatMs) {
    lastHeartbeatMs_ = millis();
    if (!connected()) {
      sessionId_ = 0;
      sendHello();
    } else {
      const uint8_t heartbeat[] = {link_protocol::kPayloadVersion};
      send(link_protocol::Kind::Event, link_protocol::Service::Core,
           link_protocol::core::kHeartbeat, heartbeat, sizeof(heartbeat));
    }
  }
}

bool Link::send(link_protocol::Kind kind, link_protocol::Service service, uint8_t opcode,
                const void* payload, size_t payloadSize, uint16_t transactionId, uint8_t flags) {
  if (payloadSize > link_protocol::kMaxPayload || (!payload && payloadSize)) return false;
  const bool busControl = service == link_protocol::Service::Core &&
      (opcode == link_protocol::core::kBusQuiesce ||
       opcode == link_protocol::core::kBusResume);
  if (transmitQuiesced_ && !busControl) return false;
  static uint8_t decoded[link_protocol::kMaxDecodedFrame];
  static uint8_t encoded[link_protocol::kMaxDecodedFrame + 8];
  std::memset(decoded, 0, sizeof(decoded));
  decoded[0] = 'O'; decoded[1] = 'S';
  decoded[2] = link_protocol::kProtocolMajor; decoded[3] = link_protocol::kProtocolMinor;
  decoded[4] = static_cast<uint8_t>(kind); decoded[5] = flags;
  decoded[6] = static_cast<uint8_t>(service); decoded[7] = opcode;
  putU16(decoded + 8, sessionId_);
  if (kind == link_protocol::Kind::Request && transactionId == 0) {
    transactionId = ++txTransaction_;
    if (transactionId == 0) transactionId = ++txTransaction_;
  }
  putU16(decoded + 10, transactionId);
  putU16(decoded + 12, ++txSequence_);
  putU16(decoded + 14, static_cast<uint16_t>(payloadSize));
  if (payloadSize) std::memcpy(decoded + link_protocol::kHeaderSize, payload, payloadSize);
  const size_t crcOffset = link_protocol::kHeaderSize + payloadSize;
  putU32(decoded + crcOffset, crc32c(decoded, crcOffset));
  const size_t encodedSize = cobsEncode(decoded, crcOffset + 4, encoded, sizeof(encoded) - 1);
  if (!encodedSize) return false;
  encoded[encodedSize] = 0;
  const size_t written = MeterBridge::instance().sendControl(encoded, encodedSize + 1)?encodedSize+1:0;
  txBytes_ += static_cast<uint32_t>(written);
  return written == encodedSize + 1;
}

void Link::sendHello() {
  ++helloCount_;
  uint8_t hello[12]{};
  hello[0] = link_protocol::kPayloadVersion;
  hello[1] = link_protocol::kProtocolMajor;
  hello[2] = link_protocol::kProtocolMinor;
  putU16(hello + 3, link_protocol::kMaxDecodedFrame);
  putU32(hello + 5, bootNonce_);
  hello[9] = 6; hello[10] = 1; hello[11] = 4;
  send(link_protocol::Kind::Request, link_protocol::Service::Core,
       link_protocol::core::kHello, hello, sizeof(hello));
}

void Link::handleDecoded(const uint8_t* decoded, size_t length) {
  ++frameAttempts_;
  if (length < link_protocol::kHeaderSize + link_protocol::kCrcSize ||
      decoded[0] != 'O' || decoded[1] != 'S' ||
      decoded[2] != link_protocol::kProtocolMajor) return;
  const uint16_t payloadSize = link_protocol::readU16(decoded + 14);
  if (payloadSize > link_protocol::kMaxPayload ||
      length != link_protocol::kHeaderSize + payloadSize + link_protocol::kCrcSize) return;
  if (link_protocol::readU32(decoded + 16 + payloadSize) != crc32c(decoded, 16 + payloadSize)) return;
  ++validFrames_;

  const uint16_t receivedSession = link_protocol::readU16(decoded + 8);
  const bool hello = decoded[6] == static_cast<uint8_t>(link_protocol::Service::Core) &&
                     decoded[7] == link_protocol::core::kHello;
  const bool helloRequest = hello && decoded[4] == static_cast<uint8_t>(link_protocol::Kind::Request);
  if (hello && receivedSession) sessionId_ = receivedSession;
  else if (helloRequest) sessionId_ = randomNonzero16();
  else if (!sessionId_ || receivedSession != sessionId_) return;
  lastRxMs_ = millis();

  const bool core = decoded[6] == static_cast<uint8_t>(link_protocol::Service::Core);
  if (core && decoded[7] == link_protocol::core::kBusQuiesce &&
      decoded[4] == static_cast<uint8_t>(link_protocol::Kind::Request) &&
      payloadSize >= 3 && decoded[16] == link_protocol::kPayloadVersion) {
    // This ACK is deliberately the last ESP->AT32 frame before silence.
    // HardwareSerial::flush() waits until the final stop bit has left GPIO21.
    transmitQuiesced_ = true;
    quiescedSinceMs_ = millis();
    quiesceLeaseMs_ = link_protocol::readU16(decoded + 17);
    if (quiesceLeaseMs_ < 100u) quiesceLeaseMs_ = 100u;
    if (quiesceLeaseMs_ > 5000u) quiesceLeaseMs_ = 5000u;
    const uint8_t ack[] = {link_protocol::kPayloadVersion};
    send(link_protocol::Kind::Response, link_protocol::Service::Core,
         link_protocol::core::kBusQuiesce, ack, sizeof(ack),
         link_protocol::readU16(decoded + 10));
    return;
  }
  if (core && decoded[7] == link_protocol::core::kBusResume &&
      decoded[4] == static_cast<uint8_t>(link_protocol::Kind::Event) &&
      payloadSize >= 1 && decoded[16] == link_protocol::kPayloadVersion) {
    transmitQuiesced_ = false;
    return;
  }

  if (handler_) {
    const ReceivedFrame frame{
      static_cast<link_protocol::Kind>(decoded[4]),
      static_cast<link_protocol::Service>(decoded[6]), decoded[7],
      link_protocol::readU16(decoded + 10), decoded + 16, payloadSize};
    handler_(frame, handlerContext_);
  }
  if (helloRequest) {
    const uint8_t response[] = {0, 1, 0, 0, 4, 6, 4};
    send(link_protocol::Kind::Response, link_protocol::Service::Core,
         link_protocol::core::kHello, response, sizeof(response),
         link_protocol::readU16(decoded + 10));
  }
}

uint32_t Link::crc32c(const uint8_t* data, size_t size) {
  uint32_t crc = 0xffffffffu;
  for (size_t i = 0; i < size; ++i) {
    crc ^= data[i];
    for (uint8_t bit = 0; bit < 8; ++bit)
      crc = (crc >> 1) ^ (0x82f63b78u & (0u - (crc & 1u)));
  }
  return ~crc;
}

size_t Link::cobsEncode(const uint8_t* input, size_t length, uint8_t* output, size_t capacity) {
  if (!capacity) return 0;
  size_t read = 0, write = 1, codeIndex = 0;
  uint8_t code = 1;
  while (read < length) {
    if (input[read] == 0) {
      output[codeIndex] = code; codeIndex = write++; code = 1; ++read;
    } else {
      if (write >= capacity) return 0;
      output[write++] = input[read++];
      if (++code == 0xff) { output[codeIndex] = code; codeIndex = write++; code = 1; }
    }
    if (write > capacity) return 0;
  }
  if (codeIndex >= capacity) return 0;
  output[codeIndex] = code;
  return write;
}

size_t Link::cobsDecode(const uint8_t* input, size_t length, uint8_t* output, size_t capacity) {
  size_t read = 0, write = 0;
  while (read < length) {
    const uint8_t code = input[read++];
    if (!code || read + code - 1 > length) return 0;
    for (uint8_t i = 1; i < code; ++i) {
      if (write >= capacity) return 0;
      output[write++] = input[read++];
    }
    if (code != 0xff && read < length) {
      if (write >= capacity) return 0;
      output[write++] = 0;
    }
  }
  return write;
}

}  // namespace openscope
