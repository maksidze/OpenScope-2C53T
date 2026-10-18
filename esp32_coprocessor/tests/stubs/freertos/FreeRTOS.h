#pragma once
#include <cstddef>
using BaseType_t=int;
using TaskHandle_t=void*;
constexpr int pdTRUE=1,pdPASS=1;
struct BridgeIterationComplete {};
inline int xTaskCreate(void (*)(void*),const char*,unsigned,void*,unsigned,TaskHandle_t* task) {
  *task=reinterpret_cast<void*>(1);return pdPASS;
}
inline void vTaskDelay(unsigned) { throw BridgeIterationComplete{}; }
