#pragma once

#include <Arduino.h>
#include <Preferences.h>
#include <WebServer.h>
#include <WiFi.h>

namespace openscope {

enum class WifiMode : uint8_t { AccessPoint = 0, Station = 1 };
enum class WifiConnection : uint8_t { AccessPoint, Applying, Connecting, Connected, NetworkNotFound, AuthenticationFailed, Disconnected };
struct WifiNetwork { String ssid; int32_t rssi = -127; bool open = false; };
struct WifiStatus {
  WifiMode configuredMode = WifiMode::AccessPoint;
  bool connected = false;
  bool fallbackAp = false;
  String apSsid;
  uint8_t apChannel = 1;
  String stationSsid;
  String ip = "192.168.4.1";
  WifiConnection connection = WifiConnection::AccessPoint;
  int32_t rssi = -127;
  uint8_t attempts = 0;
  uint8_t apClients = 0;
  bool apRunning = false;
  uint16_t disconnectReason = 0;
  String apIp = "-";
};

class WifiManager {
 public:
  static WifiManager& instance();
  void begin();
  void loop();
  WebServer& server() { return server_; }
  WifiStatus status() const { return status_; }
  bool configureStation(const String& ssid, const String& password);
  bool configureAccessPoint(const String& ssid, const String& password, uint8_t channel);
  bool setMode(WifiMode mode);
  bool startScan();
  bool scanning() const { return scanning_; }
  bool scanFailed() const { return scanFailed_; }
  uint32_t scanRevision() const { return scanRevision_; }
  size_t networkCount() const { return networkCount_; }
  const WifiNetwork& network(size_t index) const { return networks_[index]; }
  bool forgetStation();
  const String& apPassword() const { return apPassword_; }

 private:
  WifiManager() : server_(80) {}
  void load();
  bool saveMode(WifiMode mode);
  void applyConfiguredMode();
  void startAccessPoint(bool fallback);
  void startStation();
  void setupPortal();
  String defaultApSsid() const;
  void pollScan();

  Preferences preferences_;
  WebServer server_;
  WifiStatus status_{};
  String stationPassword_;
  String apPassword_ = "openscope";
  uint8_t retryCount_ = 0;
  uint32_t nextRetryMs_ = 0;
  uint32_t pendingApplyMs_ = 0;
  bool pendingApply_ = false;
  static constexpr size_t kMaxNetworks = 20;
  WifiNetwork networks_[kMaxNetworks]{};
  size_t networkCount_ = 0;
  uint32_t scanRevision_ = 0;
  bool scanning_ = false, scanFailed_ = false;
  bool preferencesReady_ = false;
  volatile uint16_t disconnectReason_ = 0;
};

}  // namespace openscope
