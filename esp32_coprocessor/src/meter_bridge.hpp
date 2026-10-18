#pragma once

#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <atomic>
#include "link_protocol.hpp"

namespace openscope {

// This task owns BOTH UARTs. Apps exchange complete envelopes through bounded
// queues, never the serial ports. Thus a blocking HTTPS GET cannot stop DVOM.
class MeterBridge {
  friend struct MeterBridgeTestAccess;
 public:
  struct Packet {
    uint16_t length;
    uint16_t wireLength;
    uint8_t data[link_protocol::kMaxDecodedFrame + 8];
  };
  static MeterBridge& instance();
  bool begin();
  bool receiveControl(Packet& packet);
  bool sendControl(const uint8_t* encoded, size_t length);
  uint32_t meterRxBytes() const { return meterRxBytes_.load(); }
  uint32_t meterTxBytes() const { return meterTxBytes_.load(); }
  uint32_t errors() const { return errors_.load(); }
  uint32_t controlDrops() const { return controlDrops_.load(); }

 private:
  MeterBridge() : host_(1), meter_(0) {}
  static void taskEntry(void* context);
  void run();
  void receiveHost();
  void dispatch(size_t length);
  bool sendMeterFrame(link_protocol::Kind kind, uint8_t opcode,
                      uint16_t transaction, const uint8_t* data, size_t length);
  HardwareSerial host_, meter_;
  QueueHandle_t controlRx_ = nullptr, controlTx_ = nullptr;
  TaskHandle_t task_ = nullptr;
  Packet incoming_{}, decoded_{}, outgoing_{};
  uint8_t txWire_[link_protocol::kMaxDecodedFrame + 8]{};
  size_t used_ = 0;
  uint32_t lastByteMs_ = 0;
  bool discard_ = false;
  std::atomic<uint32_t> meterRxBytes_{0}, meterTxBytes_{0}, errors_{0}, controlDrops_{0};
};
}  // namespace openscope
