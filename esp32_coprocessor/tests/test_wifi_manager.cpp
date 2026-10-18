// Production Wi-Fi state machine, with only the radio/NVS/HTTP edges faked.
#include "../src/wifi_manager.hpp"
#include <cstdio>
#include <cstdlib>
using namespace openscope;
static unsigned checks;
#define CHECK(x) do{++checks;if(!(x)){std::fprintf(stderr,"FAIL line %d: %s\n",__LINE__,#x);std::exit(1);}}while(0)
int main() {
  auto& wifi=WifiManager::instance();wifi.begin();
  CHECK(wifi.status().configuredMode==WifiMode::AccessPoint);
  CHECK(wifi.status().connection==WifiConnection::AccessPoint && wifi.status().apRunning);
  CHECK(wifi.status().ip=="192.168.4.1");
  CHECK(!wifi.configureStation("","12345678"));CHECK(!wifi.configureStation("router","short"));
  CHECK(!wifi.configureAccessPoint("ap","short",1));CHECK(!wifi.configureAccessPoint("ap","12345678",14));
  CHECK(!wifi.setMode(WifiMode::Station));
  CHECK(wifi.startScan() && wifi.scanning());CHECK(WiFi.currentMode==WIFI_AP_STA);
  WiFi.networks={{"weak",-95,3},{"strong",-30,0},{"weak",-15,3},{"",-10,3}};
  WiFi.scanResult=4;wifi.loop();CHECK(!wifi.scanning() && wifi.networkCount()==2);
  CHECK(wifi.network(0).ssid=="weak" && wifi.network(0).rssi==-15);
  CHECK(wifi.network(1).ssid=="strong" && wifi.network(1).open);
  CHECK(WiFi.currentMode==WIFI_AP && wifi.status().configuredMode==WifiMode::AccessPoint);
  WiFi.networks.clear();for(int i=0;i<30;i++)WiFi.networks.push_back({String("net")+String(i),-100+i,3});
  CHECK(wifi.startScan());WiFi.scanResult=30;wifi.loop();CHECK(wifi.networkCount()==20);
  CHECK(wifi.network(0).rssi==-71 && wifi.network(19).rssi==-90);
  CHECK(wifi.configureStation("router","testpass!"));CHECK(mockNvs.count("sta_cfg"));
  CHECK(wifi.status().connection==WifiConnection::Applying);CHECK(!wifi.startScan());
  mockMs+=999;wifi.loop();CHECK(WiFi.starts==0);
  ++mockMs;wifi.loop();CHECK(WiFi.starts==1 && WiFi.lastSsid=="router" && WiFi.lastPassword=="testpass!");
  CHECK(wifi.status().connection==WifiConnection::Connecting && wifi.status().attempts==1);
  WiFi.state=WL_CONNECTED;wifi.loop();CHECK(!wifi.status().connected); // no DHCP yet
  WiFi.ip={192,168,1,24};wifi.loop();CHECK(wifi.status().connected && wifi.status().rssi==-48);
  CHECK(wifi.status().connection==WifiConnection::Connected && wifi.status().ip=="192.168.1.24");
  WiFi.fail(WIFI_REASON_AUTH_FAIL);wifi.loop();CHECK(wifi.status().connection==WifiConnection::AuthenticationFailed);
  CHECK(!wifi.status().connected && wifi.status().ip=="-");
  WiFi.fail(WIFI_REASON_NO_AP_FOUND);wifi.loop();CHECK(wifi.status().connection==WifiConnection::NetworkNotFound);
  for(unsigned i=0;i<5;i++){mockMs+=15000;wifi.loop();}
  CHECK(wifi.status().fallbackAp && wifi.status().apRunning && wifi.status().configuredMode==WifiMode::Station);
  CHECK(wifi.status().ip=="-" && wifi.status().apIp=="192.168.4.1");
  WiFi.state=WL_CONNECTED;WiFi.ip={10,0,0,6};wifi.loop();CHECK(wifi.status().connected && !wifi.status().apRunning && !wifi.status().fallbackAp);
  mockNvsFailure=true;CHECK(!wifi.configureStation("new","newpass!"));CHECK(wifi.status().stationSsid=="router");
  CHECK(!wifi.setMode(WifiMode::AccessPoint));mockNvsFailure=false;
  CHECK(wifi.configureAccessPoint("Custom AP","custompass",6));mockMs+=1000;wifi.loop();
  CHECK(wifi.status().apSsid=="Custom AP" && wifi.status().apChannel==6 && wifi.apPassword()=="custompass");
  wifi.begin();CHECK(wifi.status().configuredMode==WifiMode::AccessPoint && wifi.status().apSsid=="Custom AP");
  CHECK(wifi.status().stationSsid=="router");CHECK(wifi.setMode(WifiMode::Station));mockMs+=100;wifi.loop();
  CHECK(WiFi.lastSsid=="router" && WiFi.lastPassword=="testpass!");
  CHECK(wifi.forgetStation());mockMs+=100;wifi.loop();wifi.begin();
  CHECK(wifi.status().stationSsid.isEmpty() && !mockNvs.count("sta_cfg"));
  CHECK(wifi.status().configuredMode==WifiMode::AccessPoint);
  std::printf("Production Wi-Fi manager: %u checks passed\n",checks);
}
