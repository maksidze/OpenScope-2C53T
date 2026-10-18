#pragma once

#include <Arduino.h>

namespace openscope::config {

constexpr int kScopeUartRxGpio = 20;  // <- MCU-side PA2 / USART2_TX
constexpr int kScopeUartTxGpio = 21;  // -> MCU-side PA3 / USART2_RX
constexpr uint32_t kScopeUartBaud = 115200;
constexpr int kMeterUartRxGpio = 4;  // <- meter-side TX (cut PA3 track)
constexpr int kMeterUartTxGpio = 5;  // -> meter-side RX (cut PA2 track)
constexpr uint32_t kMeterUartBaud = 9600;

constexpr int kSupplyAdcGpio = 0;
constexpr float kSupplyDividerRatio = 2.0f;  // 100k high / 100k low

constexpr uint32_t kHeartbeatMs = 500;
constexpr uint32_t kLinkTimeoutMs = 3000;
constexpr uint32_t kFrameParserTimeoutMs = 200;

constexpr uint8_t kStaRetriesBeforeFallback = 5;
constexpr uint32_t kStaRetryMs = 2000;

}  // namespace openscope::config
