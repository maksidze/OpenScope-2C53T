// Run the PRODUCTION ESP bridge and Link codec with fake UARTs/RTOS. Frames
// are generated and checked with the independent AT32 C codec, not a Python
// reimplementation. No Wi-Fi or app loop runs during the forwarding tests.
#include <atomic>
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <algorithm>
#include "Arduino.h"
#include "freertos/queue.h"
#include "driver/uart.h"
#include "../src/meter_bridge.hpp"
#include "../src/link.hpp"
#include "../src/app_runtime.hpp"
#include "../src/apps.hpp"
#include "../src/wifi_manager.hpp"
#include "../src/wifi_editor.hpp"
namespace openscope {
struct MeterBridgeTestAccess {
  static HardwareSerial& host(MeterBridge& b) { return b.host_; }
  static HardwareSerial& meter(MeterBridge& b) { return b.meter_; }
  static void run(MeterBridge& b) { b.run(); }
};
}
extern "C" {
#include "esp_link_protocol.h"
}

static unsigned checks;
#define CHECK(x) do { ++checks;if(!(x)){std::fprintf(stderr,"FAIL line %d: %s\n",__LINE__,#x);std::exit(1);} } while(0)
using Bytes=std::vector<uint8_t>;
using openscope::MeterBridge;
using Access=openscope::MeterBridgeTestAccess;

static Bytes encode(uint8_t service,uint8_t opcode,uint8_t kind,const Bytes& payload,uint16_t transaction=7) {
  oslp_frame_t f{};f.kind=kind;f.flags=OSLP_FLAG_RAW;f.service=service;
  f.opcode=opcode;f.transaction_id=transaction;f.payload=payload.data();f.payload_length=payload.size();
  uint8_t raw[1024],wire[1032];
  size_t n=oslp_frame_build(&f,raw,sizeof(raw));CHECK(n);
  n=oslp_cobs_encode(raw,n,wire,sizeof(wire)-1);CHECK(n);wire[n++]=0;
  return Bytes(wire,wire+n);
}

static void feed(MeterBridge& b,const Bytes& wire) {
  auto& rx=Access::host(b).rx;
  rx.insert(rx.end(),wire.begin(),wire.end());
}
static void step(MeterBridge& b) {
  try { Access::run(b); } catch(const BridgeIterationComplete&) {}
  mockMs++;
}
static std::vector<Bytes> unpack(const Bytes& wire,uint8_t opcode) {
  std::vector<Bytes> payloads;size_t begin=0;
  for(size_t i=0;i<wire.size();i++)if(!wire[i]) {
    uint8_t raw[1024];oslp_frame_t f{};
    size_t n=oslp_cobs_decode(wire.data()+begin,i-begin,raw,sizeof(raw));
    CHECK(n && oslp_frame_parse(raw,n,&f));
    CHECK(f.service==OSLP_SERVICE_METER_BRIDGE && f.opcode==opcode);
    if(opcode==OSLP_METER_TX_DONE)CHECK(f.kind==OSLP_KIND_ACK && f.transaction_id==7 && f.session_id==0);
    payloads.emplace_back(f.payload,f.payload+f.payload_length);begin=i+1;
  }
  CHECK(begin==wire.size());return payloads;
}
struct UiRecord { uint16_t transaction;Bytes payload; };
static UiRecord uiRecord(const Bytes& wire) {
  size_t begin=0;UiRecord result{};unsigned found=0;
  for(size_t i=0;i<wire.size();i++)if(!wire[i]) {
    uint8_t raw[1024];oslp_frame_t f{};
    size_t n=oslp_cobs_decode(wire.data()+begin,i-begin,raw,sizeof(raw));
    CHECK(n && oslp_frame_parse(raw,n,&f));begin=i+1;
    if(f.service!=OSLP_SERVICE_UI)continue;
    CHECK(f.kind==OSLP_KIND_REQUEST && f.opcode==OSLP_UI_ACTIVITY_STATE);
    result={f.transaction_id,Bytes(f.payload,f.payload+f.payload_length)};++found;
  }
  CHECK(found==1 && begin==wire.size());return result;
}

int main() {
  MeterBridge& b=MeterBridge::instance();CHECK(b.begin());
  auto& host=Access::host(b);auto& meter=Access::meter(b);
  // Opaque native payloads include all byte values and embedded OSLP-looking
  // headers. The downlink must retain every byte, regardless of inner fields.
  for(unsigned k=0;k<256;k++) {
    Bytes command{0xAA,0x55,5,static_cast<uint8_t>(k),0,0x4F,0x53,0,0,0};
    command[9]=static_cast<uint8_t>(command[2]+command[3]);
    meter.tx.clear();host.tx.clear();mockEvents.clear();
    feed(b,encode(6,1,1,command));step(b);
    CHECK(meter.tx==command);
    auto ack=unpack(host.tx,3);CHECK(ack.size()==1 && ack[0].empty());
    CHECK(mockEvents.size()>=3 && mockEvents[0]==0 && mockEvents[1]==100 && mockEvents[2]==1);
  }
  Bytes command{0xAA,0x55,5,12,0,0,0,0,0,17};
  auto good=encode(6,1,1,command);
  // Corrupted CRC, wrong payload length and wrong kind never reach the meter.
  meter.tx.clear();host.tx.clear();
  auto bad=good;bad[bad.size()-3]^=0x40;feed(b,bad);step(b);CHECK(meter.tx.empty());
  feed(b,encode(6,1,1,{1,2,3}));step(b);CHECK(meter.tx.empty());
  feed(b,encode(6,1,3,command));step(b);CHECK(meter.tx.empty());
  // Control traffic is demultiplexed, including when its queue is saturated.
  for(unsigned i=0;i<6;i++){feed(b,encode(1,1,1,command));step(b);}
  CHECK(meter.tx.empty());CHECK(b.controlDrops()==2);
  feed(b,good);step(b);CHECK(meter.tx==command);
  MeterBridge::Packet packet;unsigned received=0;
  while(b.receiveControl(packet)) {CHECK(packet.data[6]==1);received++;}
  CHECK(received==4);
  // Raw meter bytes, including unknown/partial frames, forward byte-for-byte;
  // bridge code must never apply the instrument decoder or "fix" checksums.
  host.tx.clear();Bytes expected;
  for(unsigned k=0;k<256;k++){meter.rx.push_back(k);expected.push_back(k);}
  for(unsigned i=0;i<4;i++)step(b);
  Bytes actual;for(const auto& p:unpack(host.tx,2))actual.insert(actual.end(),p.begin(),p.end());
  CHECK(actual==expected);
  // Partial and overlong envelopes are discarded; next complete frame recovers.
  meter.tx.clear();feed(b,Bytes(good.begin(),good.begin()+8));step(b);
  mockMs+=250;step(b);feed(b,good);step(b);CHECK(meter.tx.empty());
  feed(b,good);step(b);CHECK(meter.tx==command);
  meter.tx.clear();feed(b,Bytes(1500,1));feed(b,{0});feed(b,good);
  step(b);step(b);CHECK(meter.tx==command);
  // Control queue must not be loaded into an already-busy host UART. Meter
  // frames can be appended after the active envelope; the next UI stays queued.
  host.tx.clear();CHECK(b.sendControl(good.data(),good.size()));mockHostBusy=1;
  step(b);CHECK(host.tx.empty());mockHostBusy=0;step(b);CHECK(host.tx==good);
  CHECK(b.meterRxBytes()==256);CHECK(b.errors()>=3);
  std::printf("Production meter bridge: %u checks passed\n",checks);
  // UI uses stop-and-wait ACKs so a two-slot host mailbox cannot lose screen
  // definitions. Exercise the production queue, framing and retries together.
  const unsigned beforeUi=checks;
  auto& link=openscope::Link::instance();link.begin(nullptr,nullptr);
  feed(b,encode(OSLP_SERVICE_CORE,OSLP_CORE_HELLO,OSLP_KIND_REQUEST,{0}));step(b);link.loop();
  for(unsigned i=0;i<4;i++)step(b); // drain HELLO response and initial heartbeat
  CHECK(link.connected());host.tx.clear();
  openscope::UiClient ui;ui.begin(42,1,"Settings");ui.label(42,1,0,20,300,30,1,"Password");
  const char* keys[]{"abc","ABC","OK"};ui.grid(42,10,0,60,300,100,3,keys,3);ui.select(42,10,0);
  ui.loop();step(b);auto first=uiRecord(host.tx);CHECK(first.payload[1]==1 && first.transaction!=0);
  host.tx.clear();ui.loop();step(b);CHECK(host.tx.empty());
  mockMs+=800;ui.loop();step(b);auto retry=uiRecord(host.tx);
  CHECK(first.payload==retry.payload && first.transaction==retry.transaction);
  host.tx.clear();ui.acknowledge(first.transaction+1);ui.loop();step(b);CHECK(host.tx.empty());
  ui.acknowledge(first.transaction);ui.loop();step(b);auto label=uiRecord(host.tx);CHECK(label.payload[1]==2);
  host.tx.clear();ui.acknowledge(label.transaction);ui.loop();step(b);auto grid=uiRecord(host.tx);
  CHECK(grid.payload[1]==5 && grid.payload[16]==3 && grid.payload[17]==3 && grid.payload[18]==3);
  CHECK(grid.payload.size()==29); // fixed 18 bytes + three length-prefixed keys
  ui.select(42,10,1);ui.select(42,10,2); // coalesce the not-yet-sent selection
  host.tx.clear();ui.acknowledge(grid.transaction);ui.loop();step(b);auto selection=uiRecord(host.tx);
  CHECK(selection.payload[1]==0x13 && selection.payload[8]==2);
  // Changing screens retains only the in-flight copy, not obsolete drafts.
  ui.setText(42,1,"secret draft");ui.begin(42,1,"Next page");
  host.tx.clear();ui.acknowledge(selection.transaction);ui.loop();step(b);auto page=uiRecord(host.tx);
  CHECK(page.payload[1]==1 && page.transaction!=selection.transaction);
  // A different Activity token must not wait forever for an old Activity ACK.
  ui.begin(43,1,"New activity");host.tx.clear();ui.loop();step(b);auto next=uiRecord(host.tx);
  CHECK(next.payload[1]==1 && next.payload[2]==43);
  ui.acknowledge(page.transaction);host.tx.clear();ui.loop();step(b);CHECK(host.tx.empty());
  ui.reset();mockMs+=800;ui.loop();step(b);CHECK(host.tx.empty());
  std::printf("Production UI ACK/retry/coalescing: %u checks passed\n",checks-beforeUi);
  // Drive the actual Wi-Fi App using the same button values sent by AT32,
  // then inspect its production UI envelopes with the independent C codec.
  const unsigned beforeApp=checks;
  auto& wifi=openscope::WifiManager::instance();wifi.begin();
  auto& app=openscope::wifiSettingsApp();
  auto key=[&](openscope::link_protocol::Key k){app.onInput(k,openscope::link_protocol::KeyAction::Down);};
  using Key=openscope::link_protocol::Key;
  auto drain=[&]() {
    std::string text;
    for(unsigned i=0;i<100;i++) {
      host.tx.clear();ui.loop();step(b);if(host.tx.empty())return text;
      auto r=uiRecord(host.tx);text.append(r.payload.begin(),r.payload.end());ui.acknowledge(r.transaction);
    }
    CHECK(false);return text;
  };
  app.onStart(ui,100);CHECK(drain().find("Scan Wi-Fi networks")!=std::string::npos);
  key(Key::Ok);CHECK(drain().find("Scanning...")!=std::string::npos);
  WiFi.networks={{"Home",-30,3},{"Public",-50,0}};WiFi.scanResult=2;wifi.loop();app.loop();
  CHECK(drain().find("Home")!=std::string::npos);
  key(Key::Ok);CHECK(drain().find("Wi-Fi password")!=std::string::npos);
  key(Key::Save);CHECK(drain().find("8-63")!=std::string::npos);CHECK(!mockNvs.count("sta_cfg"));
  unsigned cursor=0;
  auto moveTo=[&](unsigned target) {
    while(cursor/10<target/10){key(Key::Down);cursor+=10;}
    while(cursor/10>target/10){key(Key::Up);cursor-=10;}
    while(cursor%10<target%10){key(Key::Right);++cursor;}
    while(cursor%10>target%10){key(Key::Left);--cursor;}
  };
  auto type=[&](const char* text) {
    openscope::WifiEditor layout;layout.begin("",64,true);unsigned page=0;
    for(const char* p=text;*p;p++) {
      unsigned index=40;
      for(unsigned i=0;i<40;i++)if(layout.character(i)==*p){index=i;break;}
      if(index==40) {
        page=2;moveTo(42);key(Key::Ok);layout.move(2,4);layout.press();
        for(unsigned i=0;i<40;i++)if(layout.character(i)==*p){index=i;break;}
      }
      CHECK(index<40);moveTo(index);key(Key::Ok);
    }
    (void)page;
  };
  type("testpass!");auto masked=drain();CHECK(masked.find("*********")!=std::string::npos);
  CHECK(masked.find("testpass!")==std::string::npos);
  key(Key::Save);CHECK(drain().find("Save and connect?")!=std::string::npos);
  CHECK(!mockNvs.count("sta_cfg"));key(Key::Ok);CHECK(drain().find("Saved in ESP32")!=std::string::npos);
  CHECK(mockNvs.count("sta_cfg") && wifi.status().stationSsid=="Home");
  mockMs+=1000;wifi.loop();CHECK(WiFi.lastPassword=="testpass!");
  // AP edits and manual hidden-network entry must not save on cancellation.
  const auto savedNvs=mockNvs;
  key(Key::Down);key(Key::Down);key(Key::Down);key(Key::Ok);CHECK(drain().find("Access point settings")!=std::string::npos);
  key(Key::Down);key(Key::Ok);CHECK(drain().find("Access point password")!=std::string::npos);
  key(Key::Prm);key(Key::Menu);drain();key(Key::Menu);drain();CHECK(mockNvs==savedNvs);
  key(Key::Down);key(Key::Ok);CHECK(drain().find("Network name (SSID)")!=std::string::npos);
  cursor=0;type("hidden");key(Key::Save);CHECK(drain().find("Wi-Fi password")!=std::string::npos);
  key(Key::Save);CHECK(drain().find("OPEN network - no encryption")!=std::string::npos);
  key(Key::Menu);drain();CHECK(mockNvs==savedNvs);
  app.onStop();ui.reset();std::printf("Production Wi-Fi app/button/UI flow: %u checks passed\n",checks-beforeApp);
}
