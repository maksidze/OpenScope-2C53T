#pragma once

#include <cstddef>
#include <cstdint>

namespace openscope::link_protocol {

constexpr uint8_t kProtocolMajor = 1;
constexpr uint8_t kProtocolMinor = 0;
constexpr uint8_t kPayloadVersion = 0;
constexpr size_t kHeaderSize = 16;
constexpr size_t kCrcSize = 4;
constexpr size_t kMaxDecodedFrame = 1024;
constexpr size_t kMaxPayload = kMaxDecodedFrame - kHeaderSize - kCrcSize;

enum class Kind : uint8_t { Request = 1, Response, Event, Stream, Ack, Error };
enum Flags : uint8_t {
  PayloadCbor = 1u << 0,
  PayloadRaw = 1u << 1,
  More = 1u << 2,
  Replay = 1u << 3,
  Urgent = 1u << 4,
};

enum class Service : uint8_t {
  Core = 0x00,
  Application = 0x01,
  Ui = 0x02,
  Asset = 0x03,
  Stream = 0x04,
  Device = 0x05,
  MeterBridge = 0x06,
};

namespace meter_bridge {
constexpr uint8_t kTx = 0x01;
constexpr uint8_t kRx = 0x02;
constexpr uint8_t kTxDone = 0x03;
}

namespace core {
constexpr uint8_t kHello = 0x01;
constexpr uint8_t kHeartbeat = 0x02;
constexpr uint8_t kBusQuiesce = 0x03;
constexpr uint8_t kBusResume = 0x04;
}
namespace application {
constexpr uint8_t kList = 0x01;
constexpr uint8_t kStart = 0x02;
constexpr uint8_t kStop = 0x03;
constexpr uint8_t kInput = 0x04;
}
namespace device {
constexpr uint8_t kCatalog = 0x01;
constexpr uint8_t kSubscribe = 0x02;
constexpr uint8_t kUnsubscribe = 0x03;
constexpr uint8_t kSnapshot = 0x04;
constexpr uint8_t kSetMode = 0x05;
}
namespace ui {
constexpr uint8_t kActivityMessage = 0x01;
enum class Message : uint8_t {
  ScreenBegin = 0x01,
  LabelDefine = 0x02,
  ChartDefine = 0x03,
  ScreenReady = 0x04,
  GridDefine = 0x05,
  TextSet = 0x10,
  FloatSet = 0x11,
  SeriesSet = 0x12,
  GridSelect = 0x13,
};
}

enum class Key : uint8_t {
  Ch1 = 0, Ch2, Move, Select, Trigger, Prm, Auto, Save, Menu,
  Up, Down, Left, Right, Ok, Power,
};
enum class KeyAction : uint8_t { Down = 0, Up, Repeat, Long };

inline uint16_t readU16(const uint8_t* p) {
  return static_cast<uint16_t>(p[0]) | (static_cast<uint16_t>(p[1]) << 8);
}
inline uint32_t readU32(const uint8_t* p) {
  return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
         (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24);
}

}  // namespace openscope::link_protocol
