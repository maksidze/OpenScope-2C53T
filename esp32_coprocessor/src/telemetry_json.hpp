#pragma once
#include "measurement_service.hpp"
#include <cstdarg>
#include <cstdio>
#include <cmath>

namespace openscope {
// Bounded, allocation-free JSON. Hardware strings are always escaped. Missing,
// rejected, NaN and uncalibrated quantities are null, never fabricated zeros.
class TelemetryJson {
 public:
  TelemetryJson(char* out,size_t capacity):out_(out),capacity_(capacity){if(capacity)out[0]=0;}
  void add(const char* format,...){
    if(!ok_)return;
    va_list args;va_start(args,format);
    int n=vsnprintf(out_+used_,capacity_-used_,format,args);va_end(args);
    if(n<0 || size_t(n)>=capacity_-used_){ok_=false;return;}used_+=n;
  }
  void quote(const char* s){
    add("\"");
    for(;*s && ok_;++s){
      unsigned char c=*s;
      if(c=='"' || c=='\\')add("\\%c",c);
      else if(c<32)add("\\u%04x",unsigned(c));
      else add("%c",c);
    }
    add("\"");
  }
  void number(float v,bool valid=true){if(valid && std::isfinite(v))add("%.8g",double(v));else add("null");}
  bool render(const MeasurementService& s,bool link,uint16_t session){
    const auto& m=s.meter();const auto& w=s.scope();const uint32_t now=millis();
    add("{\"api\":1,\"link\":%s,\"session\":%u,\"mode\":",link?"true":"false",unsigned(session));
    if(link && s.modeKnown())add("%u",unsigned(s.mode()));else add("null");
    add(",\"mode_age_ms\":%lu,\"streaming\":%s,\"battery_mv\":%u,\"battery_percent\":%u,"
        "\"control\":{\"pending\":%s,\"target\":%u,\"error\":%u},\"meter\":{\"available\":%s,"
        "\"age_ms\":%lu,\"revision\":%lu,\"quality\":%u,\"submode\":%u,\"result\":%u,\"reject\":%u,\"flags\":%u,\"value\":",
        static_cast<unsigned long>(s.modeAge()),s.streaming()?"true":"false",unsigned(s.batteryMv()),unsigned(s.batteryPercent()),
        s.modePending()?"true":"false",unsigned(s.requestedMode()),unsigned(s.controlError()),
        link && m.received?"true":"false",static_cast<unsigned long>(now-m.receivedMs),
        static_cast<unsigned long>(m.revision),unsigned(m.quality),unsigned(m.submode),unsigned(m.result),unsigned(m.reject),unsigned(m.flags));
    number(m.value,link && m.received && m.quality==0);
    add(",\"display\":");quote(m.display);add(",\"unit\":");quote(m.unit);
    add("},\"scope\":{\"available\":%s,\"age_ms\":%lu,\"revision\":%lu,\"flags\":%u,\"timebase\":%u,\"stride\":%u,\"sample_rate\":",
        link && w.received?"true":"false",static_cast<unsigned long>(now-w.receivedMs),static_cast<unsigned long>(w.revision),
        unsigned(w.flags),unsigned(w.timebase),unsigned(w.stride));
    number(w.sampleRate,w.sampleRate>0);
    add(",\"volts_per_count\":[");number(w.voltsPerCount[0],w.voltsPerCount[0]>0);add(",");number(w.voltsPerCount[1],w.voltsPerCount[1]>0);
    add("],\"tiers\":[%u,%u,%u],\"ch1\":[",unsigned(w.tiers[0]),unsigned(w.tiers[1]),unsigned(w.tiers[2]));
    for(unsigned ch=0;ch<2;ch++){
      if(ch)add("],\"ch2\":[");
      if(w.received && link)for(unsigned i=0;i<128;i++)add(i?",%u":"%u",unsigned(w.samples[ch][i]));
    }
    add("]}}");return ok_;
  }
 private:
  char* out_;size_t capacity_,used_=0;bool ok_=true;
};
}
