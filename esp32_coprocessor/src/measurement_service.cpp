#include "measurement_service.hpp"

#include <cmath>
#include <cstring>

namespace openscope {
namespace {
constexpr uint16_t resources[]{0x0003,0x1002,0x2001,0x0002};
constexpr uint32_t periodsUs[]{250000,250000,200000,1000000};
void put32(uint8_t* p,uint32_t v){for(unsigned i=0;i<4;i++)p[i]=v>>(8*i);}
float readFloat(const uint8_t* p){float v;memcpy(&v,p,4);return v;}
}
MeasurementService& MeasurementService::instance(){static MeasurementService service;return service;}
void MeasurementService::setEnabled(bool enabled){manualEnabled_=enabled;}
void MeasurementService::touchWeb(){webTouched_=true;webMs_=millis();}
bool MeasurementService::streaming() const {
  return manualEnabled_ || (webTouched_ && uint32_t(millis()-webMs_)<5000);
}
void MeasurementService::resetCache(){
  latest_={};meter_={};scope_={};assembling_={};parts_=0;modeKnown_=false;
  batteryMv_=0;batteryPercent_=0;
}
void MeasurementService::loop(){
  auto& link=Link::instance();
  if(!link.connected()) {
    if(session_){resetCache();session_=0;modePending_=false;controlError_=4;}
    return;
  }
  const uint32_t now=millis();const bool on=streaming();
  if(session_!=link.sessionId()) {
    session_=link.sessionId();resetCache();syncIndex_=0;renewMs_=now;
  }
  if(on!=wasStreaming_){wasStreaming_=on;syncIndex_=0;renewMs_=now;if(!on)resetCache();}
  if(on && uint32_t(now-renewMs_)>=2000){syncIndex_=0;renewMs_=now;}
  if(syncIndex_<4 && uint32_t(now-lastSubMs_)>=80) {
    uint16_t id=resources[syncIndex_];uint8_t p[8]{0,uint8_t(id),uint8_t(id>>8)};
    put32(p+3,periodsUs[syncIndex_]);p[7]=1;
    if(link.send(link_protocol::Kind::Request,link_protocol::Service::Device,
        on?link_protocol::device::kSubscribe:link_protocol::device::kUnsubscribe,p,on?8:3))++syncIndex_;
    lastSubMs_=now;
  }
  if(modePending_) {
    if(modeKnown_ && mode_==requestedMode_){modePending_=false;controlError_=0;}
    else if(uint32_t(now-modeRequestMs_)>5000){modePending_=false;controlError_=5;}
    else if(uint32_t(now-modeSendMs_)>=500) {
      uint8_t p[]{0,requestedMode_};
      link.send(link_protocol::Kind::Request,link_protocol::Service::Device,
                link_protocol::device::kSetMode,p,sizeof(p),modeTransaction_);
      modeSendMs_=now;
    }
  }
}
bool MeasurementService::requestMode(uint8_t mode){
  if(mode>3 || !Link::instance().connected() || modePending_)return false;
  touchWeb();modePending_=true;requestedMode_=mode;controlError_=0;
  if(++modeTransaction_==0)++modeTransaction_;
  modeRequestMs_=millis();modeSendMs_=millis()-500;return true;
}
void MeasurementService::handleFrame(const ReceivedFrame& frame){
  using namespace link_protocol;
  if(frame.service!=Service::Device)return;
  if(frame.opcode==device::kSetMode && modePending_ && frame.transactionId==modeTransaction_) {
    if(frame.kind==Kind::Error && frame.payloadSize>=2 && frame.payload[0]==0){
      modePending_=false;controlError_=frame.payload[1];
    }
    return; // ACK is queue acceptance; mode telemetry confirms the transition.
  }
  if(!streaming() || frame.opcode!=device::kSnapshot ||
     (frame.kind!=Kind::Event && frame.kind!=Kind::Response) ||
     frame.payloadSize<7 || frame.payload[0]!=0)return;
  const uint8_t* p=frame.payload;const uint16_t id=readU16(p+1);const uint32_t rev=readU32(p+3);
  if(id==0x0003 && frame.payloadSize==14 && p[12]==3 && p[13]<4) {
    if(!modeKnown_ || mode_!=p[13]){meter_={};scope_={};parts_=0;}
    modeKnown_=true;mode_=p[13];modeReceivedMs_=millis();
  } else if(id==0x0002 && frame.payloadSize==17 && p[12]==2) {
    batteryPercent_=p[13];batteryMv_=readU16(p+14);
  } else if(id==0x1001 && frame.payloadSize==13) {
    const float v=readFloat(p+7);if(!std::isfinite(v))return;
    latest_={id,rev,v,p[11],p[12]};
  } else if(id==0x1002 && frame.payloadSize==44) {
    if(modeKnown_ && mode_!=1)return;
    const float value=readFloat(p+16);if(!std::isfinite(value))return;
    if(!meter_.received || meter_.revision!=rev)meter_.receivedMs=millis();
    meter_.revision=rev;meter_.received=true;
    meter_.quality=p[11];meter_.submode=p[12];meter_.result=p[13];meter_.reject=p[14];meter_.flags=p[15];
    meter_.value=value;memcpy(meter_.display,p+20,15);meter_.display[15]=0;
    memcpy(meter_.unit,p+36,7);meter_.unit[7]=0;
    latest_={id,rev,value,p[12],p[11]};
  } else if(id==0x2001 && frame.payloadSize==158 && p[12]<2 && p[14]==8) {
    if(modeKnown_ && mode_!=0)return;
    float fs=readFloat(p+15),v1=readFloat(p+19),v2=readFloat(p+23);
    if(!std::isfinite(fs) || !std::isfinite(v1) || !std::isfinite(v2) || fs<0 || v1<0 || v2<0)return;
    // Never mix acquisitions after missing/reordered chunks or an AT32 reboot.
    const uint32_t stamp=readU32(p+7);
    if(!parts_ || assembling_.revision!=rev || assemblyStamp_!=stamp || uint32_t(millis()-assemblyMs_)>1000) {
      assembling_={};assembling_.revision=rev;parts_=0;assemblyMs_=millis();assemblyStamp_=stamp;
    }
    assembling_.sampleRate=fs;assembling_.voltsPerCount[0]=v1;assembling_.voltsPerCount[1]=v2;
    assembling_.flags=p[11];assembling_.timebase=p[13];assembling_.stride=p[14];
    memcpy(assembling_.tiers,p+27,3);
    memcpy(assembling_.samples[0]+p[12]*64,p+30,64);
    memcpy(assembling_.samples[1]+p[12]*64,p+94,64);parts_|=1u<<p[12];
    if(parts_==3) {
      const uint32_t received=(scope_.received && scope_.revision==rev)?scope_.receivedMs:millis();
      scope_=assembling_;scope_.received=true;scope_.receivedMs=received;parts_=0;
    }
  }
}
}
