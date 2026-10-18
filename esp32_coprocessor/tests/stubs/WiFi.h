#pragma once
#include "Arduino.h"
#include <functional>
enum wl_status_t {WL_IDLE_STATUS,WL_NO_SSID_AVAIL,WL_SCAN_COMPLETED,WL_CONNECTED,WL_CONNECT_FAILED,WL_CONNECTION_LOST,WL_DISCONNECTED};
enum {WIFI_STA=1,WIFI_AP=2,WIFI_AP_STA=3,WIFI_SCAN_RUNNING=-1,WIFI_SCAN_FAILED=-2,WIFI_AUTH_OPEN=0,WIFI_AUTH_WPA2_PSK=3};
enum {WIFI_REASON_AUTH_FAIL=202,WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT=15,WIFI_REASON_HANDSHAKE_TIMEOUT=204,WIFI_REASON_NO_AP_FOUND=201};
enum {ARDUINO_EVENT_WIFI_STA_DISCONNECTED=5};
using WiFiEvent_t=int;
struct WiFiEventInfo_t {struct {uint16_t reason;} wifi_sta_disconnected;};
class IPAddress {
 public:
  IPAddress(unsigned a=0,unsigned b=0,unsigned c=0,unsigned d=0):value_((a<<24)|(b<<16)|(c<<8)|d){}
  String toString() const {char out[16];snprintf(out,sizeof(out),"%u.%u.%u.%u",value_>>24,(value_>>16)&255,(value_>>8)&255,value_&255);return out;}
  bool operator!=(const IPAddress& other)const{return value_!=other.value_;}
 private:unsigned value_;
};
struct MockNetwork {String ssid;int32_t rssi;int security;};
class MockWifi {
 public:
  int currentMode=0,scanResult=WIFI_SCAN_RUNNING,starts=0,retries=0,clients=0;
  bool apOk=true;wl_status_t state=WL_DISCONNECTED;IPAddress ip;std::vector<MockNetwork> networks;
  String lastSsid,lastPassword;
  std::function<void(WiFiEvent_t,WiFiEventInfo_t)> disconnected;
  void persistent(bool){}
  void setAutoReconnect(bool){}
  template<typename Callback>void onEvent(Callback callback,int){disconnected=callback;}
  void mode(int value){currentMode=value;}
  bool softAP(const char*,const char*,int){return apOk;}
  IPAddress softAPIP(){return {192,168,4,1};}
  void softAPdisconnect(bool){}
  void begin(const char* ssid,const char* password){++starts;lastSsid=ssid;lastPassword=password;state=WL_DISCONNECTED;ip={};}
  wl_status_t status(){return state;}
  IPAddress localIP(){return ip;}
  int32_t RSSI(){return -48;}
  int32_t RSSI(int i){return networks.at(i).rssi;}
  unsigned softAPgetStationNum(){return clients;}
  void reconnect(){++retries;}
  void scanDelete(){}
  int scanNetworks(bool,bool,bool,int){scanResult=WIFI_SCAN_RUNNING;return scanResult;}
  int scanComplete(){return scanResult;}
  String SSID(int i){return networks.at(i).ssid;}
  int encryptionType(int i){return networks.at(i).security;}
  void fail(uint16_t reason){state=WL_DISCONNECTED;if(disconnected)disconnected(5,{{reason}});}
};
inline MockWifi WiFi;
