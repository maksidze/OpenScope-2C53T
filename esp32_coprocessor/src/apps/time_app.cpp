#include "../apps.hpp"
#include "../wifi_manager.hpp"

#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <atomic>

namespace openscope {
namespace {
class TimeApp final : public App {
 public:
  const AppManifest& manifest() const override { return manifest_; }
  void onStart(UiClient& ui, uint32_t token) override {
    ui_ = &ui; token_ = token; resultReady_.store(false);
    ui.begin(token_, manifest_.id, manifest_.name);
    ui.label(token_, 1, 8, 38, 304, 40, 2, "Ready");
    ui.label(token_, 2, 8, 88, 304, 64, 1, "-");
    ui.label(token_, 3, 8, 180, 304, 28, 1, "OK: repeat GET request");
    ui.ready(token_); fetch();
  }
  void onStop() override { ui_ = nullptr; token_ = 0; }
  void onInput(link_protocol::Key key, link_protocol::KeyAction action) override {
    if (key == link_protocol::Key::Ok && action == link_protocol::KeyAction::Down) fetch();
  }
  void loop() override {
    if (!resultReady_.exchange(false) || !ui_ || completedToken_ != token_) return;
    char status[48]; snprintf(status, sizeof(status), httpCode_ > 0 ? "HTTP %d" : "Request failed", httpCode_);
    ui_->setText(token_, 1, status); ui_->setText(token_, 2, response_);
  }
 private:
  static void taskEntry(void* context) {
    auto* self = static_cast<TimeApp*>(context);
    WiFiClientSecure client;
    // Arduino-ESP32 has no default root bundle attached to WiFiClientSecure.
    // This test endpoint therefore uses an explicitly visible development TLS
    // policy. Replace with setCACert() before treating network data as trusted.
    client.setInsecure();
    HTTPClient http;
    int code = -1;
    String body;
    if (http.begin(client, "https://timeapi.io/api/v1/time/current/unix")) {
      http.setTimeout(10000); code = http.GET();
      body = code > 0 ? http.getString() : HTTPClient::errorToString(code);
      http.end();
    } else body = "HTTP initialization failed";
    body.substring(0, sizeof(self->response_) - 1).toCharArray(self->response_, sizeof(self->response_));
    self->httpCode_ = code;
    self->completedToken_ = self->requestToken_;
    self->fetching_.store(false); self->resultReady_.store(true);
    vTaskDelete(nullptr);
  }
  void fetch() {
    if (!ui_ || fetching_.exchange(true)) return;
    if (!WifiManager::instance().status().connected) {
      fetching_.store(false); ui_->setText(token_, 1, "Wi-Fi is not connected"); return;
    }
    ui_->setText(token_, 1, "GET timeapi.io ...");
    requestToken_ = token_;
    if (xTaskCreate(taskEntry, "timeapi_get", 8192, this, 1, nullptr) != pdPASS) {
      fetching_.store(false); ui_->setText(token_, 1, "Cannot start HTTP task");
    }
  }
  const AppManifest manifest_{2, "Unix time (HTTP)", "0.1.0", "GET timeapi.io; OK refreshes"};
  std::atomic<bool> fetching_{false};
  std::atomic<bool> resultReady_{false};
  int httpCode_ = 0;
  uint32_t requestToken_ = 0;
  uint32_t completedToken_ = 0;
  char response_[256]{};
};
}
App& timeApp() { static TimeApp app; return app; }
}
