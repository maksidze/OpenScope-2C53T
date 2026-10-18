#include "wifi_manager.hpp"

#include "config.hpp"
#include "wifi_validation.hpp"
#include <esp_wifi.h>

namespace openscope {
namespace {
// One versioned NVS blob per profile keeps SSID/password/channel together.
// ESP-IDF NVS commits a single key atomically; writing separate strings could
// pair a new SSID with an old password after power loss between commits.
struct StoredWifiProfile { uint8_t version;char ssid[33],password[65];uint8_t channel; };
bool readProfile(Preferences& prefs,const char* key,StoredWifiProfile& profile,bool ap) {
  return prefs.getBytesLength(key)==sizeof(profile) &&
    prefs.getBytes(key,&profile,sizeof(profile))==sizeof(profile) && profile.version==1 &&
    profile.ssid[32]==0 && profile.password[64]==0 && validWifiSsid(profile.ssid) &&
    validWifiPassword(profile.password,!ap,!ap) && (!ap || (profile.channel>=1 && profile.channel<=13));
}
bool writeProfile(Preferences& prefs,const char* key,const String& ssid,const String& password,uint8_t channel) {
  StoredWifiProfile profile{};profile.version=1;profile.channel=channel;
  memcpy(profile.ssid,ssid.c_str(),ssid.length());memcpy(profile.password,password.c_str(),password.length());
  const bool saved=prefs.putBytes(key,&profile,sizeof(profile))==sizeof(profile);
  memset(&profile,0,sizeof(profile));return saved;
}
}

WifiManager& WifiManager::instance() { static WifiManager manager; return manager; }

String WifiManager::defaultApSsid() const {
  const uint16_t suffix = static_cast<uint16_t>(ESP.getEfuseMac());
  char name[24]; snprintf(name, sizeof(name), "OpenScope-%04X", suffix); return String(name);
}

void WifiManager::load() {
  status_.apSsid = preferences_.getString("ap_ssid", defaultApSsid());
  apPassword_ = preferences_.getString("ap_pass", "openscope");
  status_.apChannel = preferences_.getUChar("ap_channel", 1);
  if (status_.apChannel < 1 || status_.apChannel > 13) status_.apChannel = 1;
  status_.stationSsid = preferences_.getString("sta_ssid", "");
  stationPassword_ = preferences_.getString("sta_pass", "");
  StoredWifiProfile profile{};
  if(readProfile(preferences_,"ap_cfg",profile,true)) {
    status_.apSsid=profile.ssid;apPassword_=profile.password;status_.apChannel=profile.channel;
  }
  if(readProfile(preferences_,"sta_cfg",profile,false)) {status_.stationSsid=profile.ssid;stationPassword_=profile.password;}
  memset(&profile,0,sizeof(profile));
  // Keep compatibility with earlier separate keys, but never boot a malformed
  // stored AP or pass an unterminated/corrupt credential to the Wi-Fi driver.
  if(!validWifiSsid(status_.apSsid.c_str()))status_.apSsid=defaultApSsid();
  if(!validWifiPassword(apPassword_.c_str(),false,false))apPassword_="openscope";
  if(!validWifiSsid(status_.stationSsid.c_str()) || !validWifiPassword(stationPassword_.c_str(),true,true)) {
    status_.stationSsid="";stationPassword_="";
  }
  const uint8_t storedMode = preferences_.getUChar("mode", 0);
  status_.configuredMode = storedMode == 1 && !status_.stationSsid.isEmpty()
                             ? WifiMode::Station : WifiMode::AccessPoint;
}

void WifiManager::begin() {
  preferencesReady_=preferences_.begin("openscope_wifi", false);
  load();
  WiFi.persistent(false);  // Preferences is the one authoritative settings store.
  WiFi.setAutoReconnect(false);
  WiFi.onEvent([this](WiFiEvent_t, WiFiEventInfo_t info) {
    // The event task only publishes this aligned scalar. All Strings, NVS,
    // scans and mode changes remain owned by the Arduino application loop.
    disconnectReason_=info.wifi_sta_disconnected.reason;
  }, ARDUINO_EVENT_WIFI_STA_DISCONNECTED);
  applyConfiguredMode();
  setupPortal();
  server_.begin();
}

bool WifiManager::saveMode(WifiMode mode) {
  return preferences_.putUChar("mode",static_cast<uint8_t>(mode))==1;
}

void WifiManager::startAccessPoint(bool fallback) {
  if (fallback) WiFi.mode(WIFI_AP_STA);
  else WiFi.mode(WIFI_AP);
  status_.apRunning=WiFi.softAP(status_.apSsid.c_str(), apPassword_.c_str(), status_.apChannel);
  status_.fallbackAp = fallback;
  status_.connected = false;
  status_.apIp = status_.apRunning?WiFi.softAPIP().toString():String("-");
  status_.ip = fallback?String("-"):status_.apIp;
  if(!fallback)status_.connection=status_.apRunning?WifiConnection::AccessPoint:WifiConnection::Disconnected;
}

void WifiManager::startStation() {
  WiFi.softAPdisconnect(true);
  WiFi.mode(WIFI_STA);
  WiFi.begin(status_.stationSsid.c_str(), stationPassword_.c_str());
  status_.fallbackAp = false;
  status_.apRunning=false;status_.apIp="-";
  status_.connected = false;
  status_.connection=WifiConnection::Connecting;
  status_.attempts=1;disconnectReason_=0;
  status_.ip = "-";
  retryCount_ = 0;
  // Allow association, WPA handshake and DHCP to finish. A two-second restart
  // can repeatedly abort a healthy connection before it obtains an address.
  nextRetryMs_ = millis() + 15000;
}

void WifiManager::applyConfiguredMode() {
  if(scanning_) { esp_wifi_scan_stop();scanning_=false;++scanRevision_; }
  WiFi.scanDelete();
  pendingApply_ = false;
  if (status_.configuredMode == WifiMode::AccessPoint) startAccessPoint(false);
  else startStation();
}

bool WifiManager::configureStation(const String& ssid, const String& password) {
  if (!preferencesReady_ || !validWifiSsid(ssid.c_str()) ||
      !validWifiPassword(password.c_str(),true,true)) return false;
  if(!writeProfile(preferences_,"sta_cfg",ssid,password,1) || !saveMode(WifiMode::Station))return false;
  status_.stationSsid = ssid; stationPassword_ = password;
  status_.configuredMode = WifiMode::Station;
  pendingApply_ = true; pendingApplyMs_ = millis() + 1000;
  status_.connected=false;status_.connection=WifiConnection::Applying;
  return true;
}

bool WifiManager::configureAccessPoint(const String& ssid, const String& password, uint8_t channel) {
  if (!preferencesReady_ || !validWifiSsid(ssid.c_str()) ||
      !validWifiPassword(password.c_str(),false,false) || channel < 1 || channel > 13) return false;
  if(!writeProfile(preferences_,"ap_cfg",ssid,password,channel) || !saveMode(WifiMode::AccessPoint))return false;
  status_.apSsid = ssid; apPassword_ = password; status_.apChannel = channel;
  status_.configuredMode = WifiMode::AccessPoint;
  pendingApply_ = true; pendingApplyMs_ = millis() + 1000;
  status_.connected=false;status_.connection=WifiConnection::Applying;
  return true;
}

bool WifiManager::setMode(WifiMode mode) {
  if (!preferencesReady_ || (mode == WifiMode::Station && status_.stationSsid.isEmpty())) return false;
  if(!saveMode(mode))return false;
  status_.configuredMode = mode;
  pendingApply_ = true; pendingApplyMs_ = millis() + 100;
  status_.connected=false;status_.connection=WifiConnection::Applying;
  return true;
}

void WifiManager::loop() {
  server_.handleClient();
  pollScan();
  status_.apClients=status_.apRunning?WiFi.softAPgetStationNum():0;
  const uint32_t now = millis();
  if (pendingApply_ && static_cast<int32_t>(now - pendingApplyMs_) >= 0) applyConfiguredMode();
  if (status_.configuredMode != WifiMode::Station || pendingApply_) return;

  const wl_status_t linkStatus=WiFi.status();
  if (linkStatus == WL_CONNECTED && WiFi.localIP()!=IPAddress(0,0,0,0)) {
    if (!status_.connected || status_.fallbackAp) {
      status_.connected = true;
      status_.ip = WiFi.localIP().toString();
      retryCount_ = 0;
      status_.connection=WifiConnection::Connected;
      status_.disconnectReason=0;
      if (status_.fallbackAp) {
        WiFi.softAPdisconnect(true);
        WiFi.mode(WIFI_STA);
        status_.fallbackAp = false;
        status_.apRunning=false;status_.apIp="-";
      }
    }
    status_.rssi=WiFi.RSSI();
    return;
  }

  status_.connected = false;
  status_.ip="-";status_.rssi=-127;status_.disconnectReason=disconnectReason_;
  if(disconnectReason_==WIFI_REASON_AUTH_FAIL || disconnectReason_==WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT ||
     disconnectReason_==WIFI_REASON_HANDSHAKE_TIMEOUT)
    status_.connection=WifiConnection::AuthenticationFailed;
  else if(linkStatus==WL_NO_SSID_AVAIL || disconnectReason_==WIFI_REASON_NO_AP_FOUND)
    status_.connection=WifiConnection::NetworkNotFound;
  else if(linkStatus==WL_CONNECT_FAILED)status_.connection=WifiConnection::Disconnected;
  else status_.connection=WifiConnection::Connecting;
  if(scanning_)return;
  if (static_cast<int32_t>(now - nextRetryMs_) < 0) return;
  nextRetryMs_ = now + 15000;
  if (!status_.fallbackAp && ++retryCount_ >= config::kStaRetriesBeforeFallback) {
    startAccessPoint(true);
  }
  WiFi.reconnect();
  if(status_.attempts<255)++status_.attempts;
}

bool WifiManager::forgetStation() {
  if(!preferencesReady_)return false;
  if(!saveMode(WifiMode::AccessPoint))return false;
  for(const char* key:{"sta_cfg","sta_ssid","sta_pass"})
    if(preferences_.isKey(key) && !preferences_.remove(key))return false;
  status_.stationSsid="";stationPassword_="";
  return setMode(WifiMode::AccessPoint);
}

bool WifiManager::startScan() {
  if(scanning_)return true;
  if(pendingApply_){scanFailed_=true;networkCount_=0;++scanRevision_;return false;}
  WiFi.scanDelete();networkCount_=0;scanFailed_=false;
  // STA must be enabled for scanning. AP mode is restored after the scan;
  // the configured/saved mode is never changed by discovering networks.
  if(status_.configuredMode==WifiMode::AccessPoint)WiFi.mode(WIFI_AP_STA);
  const int result=WiFi.scanNetworks(true,true,false,200);
  scanning_=result==WIFI_SCAN_RUNNING;
  if(!scanning_) { scanFailed_=true;++scanRevision_; }
  return scanning_;
}

void WifiManager::pollScan() {
  if(!scanning_)return;
  const int count=WiFi.scanComplete();
  if(count==WIFI_SCAN_RUNNING)return;
  scanning_=false;scanFailed_=count<0;
  for(int i=0;i<count;i++) {
    const String ssid=WiFi.SSID(i);
    if(ssid.isEmpty())continue; // hidden networks use the manual-SSID editor.
    const int32_t rssi=WiFi.RSSI(i);bool duplicate=false;
    for(size_t j=0;j<networkCount_;j++)if(networks_[j].ssid==ssid) {
      if(rssi>networks_[j].rssi)networks_[j]={ssid,rssi,WiFi.encryptionType(i)==WIFI_AUTH_OPEN};
      duplicate=true;break;
    }
    if(duplicate)continue;
    size_t slot=networkCount_;
    if(slot==kMaxNetworks) { slot=kMaxNetworks-1;if(rssi<=networks_[slot].rssi)continue; }
    else ++networkCount_;
    networks_[slot]={ssid,rssi,WiFi.encryptionType(i)==WIFI_AUTH_OPEN};
    for(size_t j=slot;j>0 && networks_[j].rssi>networks_[j-1].rssi;j--) {
      WifiNetwork tmp=networks_[j-1];networks_[j-1]=networks_[j];networks_[j]=tmp;
    }
  }
  // A stronger duplicate may have replaced an entry in the middle of the
  // list; sort once more after deduplication so paging stays in RSSI order.
  for(size_t i=1;i<networkCount_;i++)for(size_t j=i;j>0 && networks_[j].rssi>networks_[j-1].rssi;j--) {
    WifiNetwork tmp=networks_[j-1];networks_[j-1]=networks_[j];networks_[j]=tmp;
  }
  WiFi.scanDelete();++scanRevision_;
  if(status_.configuredMode==WifiMode::AccessPoint && !pendingApply_)WiFi.mode(WIFI_AP);
}

void WifiManager::setupPortal() {
  server_.on("/wifi-settings", HTTP_GET, [this]() {
    const String page =
      F("<!doctype html><meta name=viewport content='width=device-width'><title>OpenScope Wi-Fi</title>"
        "<style>body{font:18px sans-serif;max-width:32em;margin:3em auto;padding:1em}"
        "input,button{font:inherit;width:100%;margin:.4em 0;padding:.5em;box-sizing:border-box}</style>"
        "<a href='/'>OpenScope dashboard</a><h1>OpenScope Wi-Fi</h1><h2>Connect to Wi-Fi</h2>"
        "<form method=post action=/wifi><label>SSID<input name=ssid maxlength=32></label>"
        "<label>Password<input name=password type=password maxlength=64></label>"
        "<button>Save and connect</button></form><h2>Access point</h2>"
        "<form method=post action=/ap><label>AP name<input name=ssid maxlength=32></label>"
        "<label>AP password (8-63 chars)<input name=password type=password maxlength=63></label>"
        "<label>Channel<input name=channel type=number min=1 max=13 value=1></label>"
        "<button>Save and use AP mode</button></form>");
    server_.send(200, "text/html; charset=utf-8", page);
  });
  server_.on("/wifi", HTTP_POST, [this]() {
    if (!configureStation(server_.arg("ssid"), server_.arg("password"))) {
      server_.send(400, "text/plain", "Invalid station settings"); return;
    }
    server_.send(200, "text/plain", "Saved. Switching to station mode in one second; fallback AP starts only after bounded retries.");
  });
  server_.on("/ap", HTTP_POST, [this]() {
    const int channel = server_.arg("channel").toInt();
    if (!configureAccessPoint(server_.arg("ssid"), server_.arg("password"), channel)) {
      server_.send(400, "text/plain", "AP password must be 8-63 characters; channel 1-13"); return;
    }
    server_.send(200, "text/plain", "AP settings saved. Restarting AP in one second.");
  });
  server_.onNotFound([this]() { server_.sendHeader("Location", "/", true); server_.send(302); });
}

}  // namespace openscope
