#pragma once
#include "FreeRTOS.h"
#include <vector>
#include <deque>
#include <cstring>
struct MockQueue { size_t capacity,size;std::deque<std::vector<uint8_t>> items; };
using QueueHandle_t=MockQueue*;
inline QueueHandle_t xQueueCreate(size_t count,size_t size) { return new MockQueue{count,size,{}}; }
inline void vQueueDelete(QueueHandle_t q) { delete q; }
inline int xQueueSend(QueueHandle_t q,const void* value,unsigned) {
  if(q->items.size()==q->capacity)return 0;
  const auto* p=static_cast<const uint8_t*>(value);
  q->items.emplace_back(p,p+q->size);return pdTRUE;
}
inline int xQueueReceive(QueueHandle_t q,void* value,unsigned) {
  if(q->items.empty())return 0;
  memcpy(value,q->items.front().data(),q->size);q->items.pop_front();return pdTRUE;
}
