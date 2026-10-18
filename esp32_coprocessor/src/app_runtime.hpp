#pragma once

#include <Arduino.h>
#include "link.hpp"

namespace openscope {

struct AppManifest {
  uint16_t id;
  const char* name;
  const char* version;
  const char* description;
};

class UiClient {
 public:
  void begin(uint32_t token, uint16_t appId, const char* title);
  void label(uint32_t token, uint16_t subject, int16_t x, int16_t y,
             int16_t width, int16_t height, uint8_t font, const char* text);
  void chart(uint32_t token, uint16_t subject, int16_t x, int16_t y,
             int16_t width, int16_t height, uint16_t capacity);
  void ready(uint32_t token);
  void setText(uint32_t token, uint16_t subject, const char* text);
  void setFloat(uint32_t token, uint16_t subject, float value, const char* unit);
  void setSeries(uint32_t token, uint16_t subject, const int16_t* values, size_t count);
  void grid(uint32_t token, uint16_t subject, int16_t x, int16_t y,
            int16_t width, int16_t height, uint8_t columns, const char* const* keys, size_t count);
  void select(uint32_t token, uint16_t subject, uint16_t index);
  void loop();
  void acknowledge(uint16_t transaction);
  void reset();
 private:
  bool enqueue(const uint8_t* data, size_t size);
  void discardPending();
  static constexpr size_t kQueueSize=24, kRecordSize=384;
  struct Record { uint16_t length,transaction;uint8_t data[kRecordSize]; } records_[kQueueSize]{};
  uint8_t head_=0,tail_=0,count_=0;
  uint16_t nextTransaction_=0;
  uint32_t sentMs_=0;
  uint32_t currentToken_=0;
  bool waiting_=false;
};

class App {
 public:
  virtual ~App() = default;
  virtual const AppManifest& manifest() const = 0;
  virtual void onStart(UiClient& ui, uint32_t token) = 0;
  virtual void onStop() = 0;
  virtual void onInput(link_protocol::Key key, link_protocol::KeyAction action) = 0;
  virtual void loop() = 0;
 protected:
  UiClient* ui_ = nullptr;
  uint32_t token_ = 0;
};

class AppRuntime {
 public:
  static AppRuntime& instance();
  void registerApp(App* app);
  void begin();
  void loop();
  void handleFrame(const ReceivedFrame& frame);
  void closeActive();

 private:
  static void linkFrameHandler(const ReceivedFrame& frame, void* context);
  void sendManifestList(uint16_t transactionId);
  App* find(uint16_t id) const;
  static constexpr size_t kMaxApps = 8;
  App* apps_[kMaxApps]{};
  size_t appCount_ = 0;
  App* active_ = nullptr;
  uint32_t activityToken_ = 0;
  uint32_t lastAppTickMs_ = 0;
  uint32_t closingToken_ = 0, closeSentMs_ = 0;
  UiClient ui_;
};

}  // namespace openscope
