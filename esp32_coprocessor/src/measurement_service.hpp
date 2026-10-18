#pragma once

#include "link.hpp"

namespace openscope {
struct MeasurementSnapshot { uint16_t resourceId=0; uint32_t revision=0; float value=0; uint8_t unit=0,quality=6; };
struct MeterSnapshot {
  uint32_t revision=0,receivedMs=0;
  float value=0;
  uint8_t quality=6,submode=0,result=0,reject=0,flags=0;
  bool received=false;
  char display[16]{},unit[8]{};
};
struct ScopeSnapshot {
  uint32_t revision=0,receivedMs=0;
  float sampleRate=0,voltsPerCount[2]{};
  uint8_t flags=0,timebase=0,stride=8,tiers[3]{};
  bool received=false;
  uint8_t samples[2][128]{};
};

// Arduino loop owns this background service; foreground and web are separate leases.
class MeasurementService {
 public:
  static MeasurementService& instance();
  void setEnabled(bool enabled);
  bool enabled() const { return manualEnabled_; }
  void touchWeb();
  bool streaming() const;
  void loop();
  void handleFrame(const ReceivedFrame& frame);
  MeasurementSnapshot latest() const { return latest_; }
  const MeterSnapshot& meter() const { return meter_; }
  const ScopeSnapshot& scope() const { return scope_; }
  bool modeKnown() const { return modeKnown_; }
  uint8_t mode() const { return mode_; }
  uint32_t modeAge() const { return millis()-modeReceivedMs_; }
  uint8_t batteryPercent() const { return batteryPercent_; }
  uint16_t batteryMv() const { return batteryMv_; }
  bool requestMode(uint8_t mode);
  bool modePending() const { return modePending_; }
  uint8_t requestedMode() const { return requestedMode_; }
  uint8_t controlError() const { return controlError_; }
 private:
  void resetCache();
  bool manualEnabled_=false,webTouched_=false,wasStreaming_=false,modeKnown_=false;
  uint32_t webMs_=0,renewMs_=0,lastSubMs_=0,modeReceivedMs_=0;
  uint16_t session_=0;
  uint8_t syncIndex_=4,mode_=0,batteryPercent_=0;
  uint16_t batteryMv_=0;
  MeasurementSnapshot latest_{};
  MeterSnapshot meter_{};
  ScopeSnapshot scope_{},assembling_{};
  uint8_t parts_=0;
  uint32_t assemblyMs_=0,assemblyStamp_=0;
  bool modePending_=false;
  uint8_t requestedMode_=0,controlError_=0;
  uint32_t modeRequestMs_=0,modeSendMs_=0;
  uint16_t modeTransaction_=0xD000;
};
}
