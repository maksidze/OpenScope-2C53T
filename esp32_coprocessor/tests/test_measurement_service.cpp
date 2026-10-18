// Production background service + HTTP routes with only transport/HTTP edges
// faked. This exercises decoder bounds, coherence, leases and mode confirmation.
#include "../src/measurement_service.hpp"
#include "../src/instrument_web.hpp"
#include "../src/telemetry_json.hpp"
#include "../src/app_runtime.hpp"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <vector>
using namespace openscope;
using namespace openscope::link_protocol;
struct Sent {uint8_t opcode;std::vector<uint8_t> payload;uint16_t transaction;};
static std::vector<Sent> sent;
static bool online=true;
static unsigned foregroundCloses=0;
namespace openscope {
AppRuntime& AppRuntime::instance(){static AppRuntime runtime;return runtime;}
void AppRuntime::closeActive(){++foregroundCloses;}
Link& Link::instance(){static Link l;return l;}
bool Link::connected() const{return online;}
void Link::begin(FrameHandler,void*){++sessionId_;}
bool Link::send(Kind,Service,uint8_t op,const void* p,size_t n,uint16_t transaction,uint8_t){
  const auto* b=static_cast<const uint8_t*>(p);sent.push_back({op,{b,b+n},transaction});return true;
}
}
static unsigned checks;
#define CHECK(x) do{++checks;if(!(x)){std::fprintf(stderr,"FAIL line %d: %s\n",__LINE__,#x);std::exit(1);}}while(0)
static void u32(uint8_t* p,uint32_t n){for(unsigned i=0;i<4;i++)p[i]=n>>(8*i);}
static void f32(uint8_t* p,float v){memcpy(p,&v,4);}
static void snapshot(const std::vector<uint8_t>& p,Kind kind=Kind::Event){
  MeasurementService::instance().handleFrame({kind,Service::Device,device::kSnapshot,0,p.data(),p.size()});
}
static void mode(uint8_t n){std::vector<uint8_t> p(14);p[1]=3;p[12]=3;p[13]=n;snapshot(p);}
static std::vector<uint8_t> part(uint32_t revision,uint8_t index){
  std::vector<uint8_t> p(158);p[1]=1;p[2]=0x20;u32(p.data()+3,revision);p[11]=31;p[12]=index;p[13]=16;p[14]=8;
  f32(p.data()+15,12500);f32(p.data()+19,.1f);f32(p.data()+23,.2f);p[27]=p[28]=p[29]=2;
  for(unsigned i=0;i<64;i++){p[30+i]=index*64+i;p[94+i]=255-index*64-i;}return p;
}
int main(){
  auto& s=MeasurementService::instance();Link::instance().begin(nullptr,nullptr);mockMs=100;
  CHECK(!s.streaming());s.touchWeb();s.loop();CHECK(s.streaming());
  for(unsigned i=0;i<4;i++){mockMs+=80;s.loop();}
  CHECK(sent.size()==4);
  CHECK(sent[0].opcode==device::kSubscribe && sent[0].payload[1]==3);
  CHECK(sent[1].payload[1]==2 && sent[1].payload[2]==0x10);
  CHECK(sent[2].payload[1]==1 && sent[2].payload[2]==0x20);
  mode(0);CHECK(s.modeKnown() && s.mode()==0);
  snapshot(part(20,0));CHECK(!s.scope().received);
  snapshot(part(22,1));CHECK(!s.scope().received);
  snapshot(part(22,0));CHECK(s.scope().received && s.scope().revision==22);
  CHECK(s.scope().samples[0][0]==0 && s.scope().samples[0][127]==127);
  CHECK(s.scope().samples[1][0]==255 && s.scope().samples[1][127]==128);
  uint32_t received=s.scope().receivedMs;mockMs+=100;snapshot(part(22,0));snapshot(part(22,1));
  CHECK(s.scope().receivedMs==received); // unchanged ADC generation ages honestly
  auto bad=part(24,0);bad[12]=2;snapshot(bad);CHECK(s.scope().revision==22);
  bad=part(24,0);f32(bad.data()+15,std::numeric_limits<float>::quiet_NaN());snapshot(bad);snapshot(part(24,1));CHECK(s.scope().revision==22);
  snapshot(part(24,0));CHECK(s.scope().revision==24);
  auto mismatched0=part(26,0),mismatched1=part(26,1);u32(mismatched1.data()+7,10);
  snapshot(mismatched0);snapshot(mismatched1);CHECK(s.scope().revision==24);
  snapshot(part(24,0));snapshot(part(24,1));
  // Every truncated packet is ignored without touching cache or reading OOB.
  for(unsigned n=0;n<158;n++){auto shortPart=part(30,0);shortPart.resize(n);snapshot(shortPart);CHECK(s.scope().revision==24);}
  snapshot(part(32,0),Kind::Request);snapshot(part(32,1),Kind::Request);CHECK(s.scope().revision==24);
  mode(1);CHECK(!s.scope().received);
  std::vector<uint8_t> meter(44);meter[1]=2;meter[2]=0x10;u32(meter.data()+3,100);meter[12]=6;meter[13]=1;
  f32(meter.data()+16,4.7f);memcpy(meter.data()+20,"4.700",6);memcpy(meter.data()+36,"kOhm",5);
  snapshot(meter);CHECK(s.meter().received && s.meter().quality==0);
  CHECK(std::string(s.meter().unit)=="kOhm" && std::string(s.meter().display)=="4.700");
  received=s.meter().receivedMs;mockMs+=100;snapshot(meter);CHECK(s.meter().receivedMs==received);
  char json[3072];TelemetryJson j(json,sizeof(json));CHECK(j.render(s,true,1));
  CHECK(std::string(json).find("\"unit\":\"kOhm\"")!=std::string::npos);
  meter[11]=2;meter[13]=5;u32(meter.data()+3,101);snapshot(meter);
  TelemetryJson ol(json,sizeof(json));CHECK(ol.render(s,true,1));CHECK(std::string(json).find("\"value\":null")!=std::string::npos);
  std::fill(meter.begin()+20,meter.begin()+36,0);memcpy(meter.data()+20,"\"\\\n",3);snapshot(meter);
  TelemetryJson escaped(json,sizeof(json));CHECK(escaped.render(s,true,1));CHECK(std::string(json).find("\\u000a")!=std::string::npos);
  char tiny[8];TelemetryJson overflow(tiny,sizeof(tiny));CHECK(!overflow.render(s,true,1));
  mode(0);snapshot(part(40,0));snapshot(part(40,1));TelemetryJson scopeJson(json,sizeof(json));CHECK(scopeJson.render(s,true,1));
  CHECK(std::strlen(json)<sizeof(json));CHECK(std::string(json).find("\"sample_rate\":12500")!=std::string::npos);
  CHECK(!s.requestMode(4));CHECK(s.requestMode(1));CHECK(!s.requestMode(2));s.loop();CHECK(s.modePending());
  const auto firstTransaction=sent.back().transaction;
  uint8_t ack[]{0,1};s.handleFrame({Kind::Ack,Service::Device,device::kSetMode,firstTransaction,ack,2});CHECK(s.modePending());
  mode(1);s.loop();CHECK(!s.modePending() && s.controlError()==0);
  CHECK(s.requestMode(0));s.loop();uint8_t error[]{0,3};
  s.handleFrame({Kind::Error,Service::Device,device::kSetMode,firstTransaction,error,2});CHECK(s.modePending());
  s.handleFrame({Kind::Error,Service::Device,device::kSetMode,sent.back().transaction,error,2});
  CHECK(!s.modePending() && s.controlError()==3);
  CHECK(s.requestMode(0));mockMs+=5001;s.loop();CHECK(!s.modePending() && s.controlError()==5);
  CHECK(!s.streaming());for(unsigned i=0;i<4;i++){mockMs+=80;s.loop();}CHECK(sent.back().opcode==device::kUnsubscribe);
  s.setEnabled(true);CHECK(s.enabled() && s.streaming());s.touchWeb();s.setEnabled(false);CHECK(!s.enabled() && s.streaming());
  online=false;s.loop();CHECK(!s.modeKnown() && !s.meter().received && !s.scope().received);
  online=true;Link::instance().begin(nullptr,nullptr);s.loop();mode(0);snapshot(part(2,0));snapshot(part(2,1));
  CHECK(s.scope().revision==2); // reboot starts a new revision domain
  WebServer server(80);InstrumentWeb::begin(server);
  server.invoke("/",HTTP_GET);CHECK(server.responseCode==200 && server.responseBody.find("<canvas")!=std::string::npos);
  server.invoke("/api/live",HTTP_GET);CHECK(server.responseCode==200 && server.responseBody.find("\"api\":1")!=std::string::npos);
  server.args["mode"]="1";server.invoke("/api/mode",HTTP_POST);CHECK(server.responseCode==403);
  CHECK(foregroundCloses==0);
  server.headers["X-OpenScope-Control"]="1";server.headers["Origin"]="http://evil.invalid";
  server.invoke("/api/mode",HTTP_POST);CHECK(server.responseCode==403);
  server.headers["Origin"]="http://192.168.1.200";
  for(const char* invalid:{"","foo","-1","1x","4","100"}){
    server.args["mode"]=invalid;server.invoke("/api/mode",HTTP_POST);CHECK(server.responseCode==400);
  }
  server.args["mode"]="1";server.invoke("/api/mode",HTTP_POST);CHECK(server.responseCode==202);
  CHECK(foregroundCloses==1);
  server.invoke("/api/mode",HTTP_POST);CHECK(server.responseCode==409);
  online=false;server.invoke("/api/mode",HTTP_POST);CHECK(server.responseCode==503);
  std::printf("Production telemetry/HTTP: %u checks passed\n",checks);
}
