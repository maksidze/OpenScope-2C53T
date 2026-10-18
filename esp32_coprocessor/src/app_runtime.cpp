#include "app_runtime.hpp"

#include <algorithm>
#include <cstring>
#include "measurement_service.hpp"

namespace openscope {
namespace {
class Writer {
 public:
  Writer(uint8_t* data, size_t capacity) : data_(data), capacity_(capacity) {}
  bool u8(uint8_t v) { return bytes(&v, 1); }
  bool u16(uint16_t v) { const uint8_t b[]{uint8_t(v), uint8_t(v >> 8)}; return bytes(b, 2); }
  bool u32(uint32_t v) { return u16(v) && u16(v >> 16); }
  bool i16(int16_t v) { return u16(static_cast<uint16_t>(v)); }
  bool f32(float v) { return bytes(&v, sizeof(v)); }
  bool string(const char* value, size_t maximum = 255) {
    if (!value) value = "";
    const size_t length = std::min(strlen(value), maximum);
    return u8(static_cast<uint8_t>(length)) && bytes(value, length);
  }
  bool bytes(const void* value, size_t length) {
    if (used_ + length > capacity_) return false;
    if (length) memcpy(data_ + used_, value, length);
    used_ += length; return true;
  }
  size_t size() const { return used_; }
 private:
  uint8_t* data_; size_t capacity_; size_t used_ = 0;
};

}

bool UiClient::enqueue(const uint8_t* data,size_t size) {
  if(size>kRecordSize)return false;
  // Coalesce unsent updates to one subject. Never modify the in-flight record:
  // its transaction identifies exactly the bytes already applied by the host.
  if(size>=8 && data[1]>=0x10) {
    for(unsigned i=waiting_?1:0;i<count_;i++) {
      Record& r=records_[(tail_+i)%kQueueSize];
      if(r.length>=8 && r.data[1]==data[1] && memcmp(r.data+2,data+2,6)==0) {
        memcpy(r.data,data,size);r.length=size;return true;
      }
    }
  }
  if(count_==kQueueSize)return false;
  Record& r=records_[head_];r.length=size;
  if(++nextTransaction_==0)++nextTransaction_;
  r.transaction=nextTransaction_;memcpy(r.data,data,size);
  head_=(head_+1)%kQueueSize;++count_;return true;
}
void UiClient::discardPending() {
  for(unsigned i=waiting_?1:0;i<count_;i++)memset(&records_[(tail_+i)%kQueueSize],0,sizeof(Record));
  if(waiting_ && count_) { count_=1;head_=(tail_+1)%kQueueSize; }
  else { head_=tail_=count_=0;waiting_=false; }
}
void UiClient::reset() {
  memset(records_,0,sizeof(records_));head_=tail_=count_=0;waiting_=false;currentToken_=0;
}
void UiClient::acknowledge(uint16_t transaction) {
  if(!waiting_ || !count_ || records_[tail_].transaction!=transaction)return;
  // Erase text drafts, including a deliberately revealed password, when the
  // transport no longer needs its retry copy. Credentials are never logged.
  memset(&records_[tail_],0,sizeof(Record));
  tail_=(tail_+1)%kQueueSize;--count_;waiting_=false;
}
void UiClient::loop() {
  if(!count_ || !Link::instance().connected())return;
  if(waiting_ && static_cast<uint32_t>(millis()-sentMs_)<800)return;
  const Record& r=records_[tail_];
  if(Link::instance().send(link_protocol::Kind::Request,link_protocol::Service::Ui,
      link_protocol::ui::kActivityMessage,r.data,r.length,r.transaction)) {
    waiting_=true;sentMs_=millis();
  }
}

void UiClient::begin(uint32_t token, uint16_t appId, const char* title) {
  if(token!=currentToken_)reset();else discardPending();
  currentToken_=token;
  uint8_t p[128]{}; Writer w(p, sizeof(p));
  w.u8(0); w.u8(uint8_t(link_protocol::ui::Message::ScreenBegin));
  w.u32(token); w.u16(appId); w.string(title, 80); enqueue(p, w.size());
}
void UiClient::label(uint32_t token, uint16_t subject, int16_t x, int16_t y,
                     int16_t width, int16_t height, uint8_t font, const char* text) {
  uint8_t p[256]{}; Writer w(p, sizeof(p));
  w.u8(0); w.u8(uint8_t(link_protocol::ui::Message::LabelDefine)); w.u32(token); w.u16(subject);
  w.i16(x); w.i16(y); w.i16(width); w.i16(height); w.u8(font); w.string(text, 180); enqueue(p, w.size());
}
void UiClient::chart(uint32_t token, uint16_t subject, int16_t x, int16_t y,
                     int16_t width, int16_t height, uint16_t capacity) {
  uint8_t p[32]{}; Writer w(p, sizeof(p));
  w.u8(0); w.u8(uint8_t(link_protocol::ui::Message::ChartDefine)); w.u32(token); w.u16(subject);
  w.i16(x); w.i16(y); w.i16(width); w.i16(height); w.u16(capacity); enqueue(p, w.size());
}
void UiClient::ready(uint32_t token) {
  uint8_t p[6]{}; Writer w(p, sizeof(p)); w.u8(0);
  w.u8(uint8_t(link_protocol::ui::Message::ScreenReady)); w.u32(token); enqueue(p, w.size());
}
void UiClient::setText(uint32_t token, uint16_t subject, const char* text) {
  uint8_t p[256]{}; Writer w(p, sizeof(p)); w.u8(0);
  w.u8(uint8_t(link_protocol::ui::Message::TextSet)); w.u32(token); w.u16(subject);
  w.string(text, 220); enqueue(p, w.size());
}
void UiClient::setFloat(uint32_t token, uint16_t subject, float value, const char* unit) {
  uint8_t p[32]{}; Writer w(p, sizeof(p)); w.u8(0);
  w.u8(uint8_t(link_protocol::ui::Message::FloatSet)); w.u32(token); w.u16(subject);
  w.f32(value); w.string(unit, 12); enqueue(p, w.size());
}
void UiClient::setSeries(uint32_t token, uint16_t subject, const int16_t* values, size_t count) {
  count = std::min<size_t>(count, 180);
  uint8_t p[512]{}; Writer w(p, sizeof(p)); w.u8(0);
  w.u8(uint8_t(link_protocol::ui::Message::SeriesSet)); w.u32(token); w.u16(subject);
  w.u16(static_cast<uint16_t>(count)); w.bytes(values, count * sizeof(*values)); enqueue(p, w.size());
}

void UiClient::grid(uint32_t token,uint16_t subject,int16_t x,int16_t y,
                    int16_t width,int16_t height,uint8_t columns,const char* const* keys,size_t count) {
  if(!columns || columns>10 || !count || count>50)return;
  uint8_t p[kRecordSize]{};Writer w(p,sizeof(p));w.u8(0);
  w.u8(uint8_t(link_protocol::ui::Message::GridDefine));w.u32(token);w.u16(subject);
  w.i16(x);w.i16(y);w.i16(width);w.i16(height);w.u8(columns);w.u8(count);
  for(size_t i=0;i<count;i++)if(!w.string(keys[i],60))return;
  enqueue(p,w.size());
}
void UiClient::select(uint32_t token,uint16_t subject,uint16_t index) {
  uint8_t p[10]{};Writer w(p,sizeof(p));w.u8(0);
  w.u8(uint8_t(link_protocol::ui::Message::GridSelect));w.u32(token);w.u16(subject);w.u16(index);
  enqueue(p,w.size());
}

AppRuntime& AppRuntime::instance() { static AppRuntime runtime; return runtime; }
void AppRuntime::registerApp(App* app) { if (app && appCount_ < kMaxApps) apps_[appCount_++] = app; }
void AppRuntime::begin() { Link::instance().begin(linkFrameHandler, this); }
void AppRuntime::loop() {
  Link::instance().loop();
  MeasurementService::instance().loop();
  ui_.loop();
  if(closingToken_ && static_cast<uint32_t>(millis()-closeSentMs_)>=500) {
    const uint8_t p[]{0,uint8_t(closingToken_),uint8_t(closingToken_>>8),uint8_t(closingToken_>>16),uint8_t(closingToken_>>24)};
    Link::instance().send(link_protocol::Kind::Request,link_protocol::Service::Application,
      link_protocol::application::kStop,p,sizeof(p),0xFFFE);
    closeSentMs_=millis();
  }
  if (active_ && static_cast<uint32_t>(millis() - lastAppTickMs_) >= 50) {
    lastAppTickMs_ = millis(); active_->loop();
  }
}
void AppRuntime::linkFrameHandler(const ReceivedFrame& frame, void* context) {
  static_cast<AppRuntime*>(context)->handleFrame(frame);
}
App* AppRuntime::find(uint16_t id) const {
  for (size_t i = 0; i < appCount_; ++i) if (apps_[i]->manifest().id == id) return apps_[i];
  return nullptr;
}
void AppRuntime::sendManifestList(uint16_t transactionId) {
  uint8_t p[link_protocol::kMaxPayload]{}; Writer w(p, sizeof(p)); w.u8(0); w.u8(appCount_);
  for (size_t i = 0; i < appCount_; ++i) {
    const auto& m = apps_[i]->manifest();
    if (!w.u16(m.id) || !w.string(m.name, 48) || !w.string(m.version, 16) || !w.string(m.description, 96)) break;
  }
  Link::instance().send(link_protocol::Kind::Response, link_protocol::Service::Application,
                        link_protocol::application::kList, p, w.size(), transactionId);
}
void AppRuntime::handleFrame(const ReceivedFrame& frame) {
  if(frame.service==link_protocol::Service::Ui && frame.kind==link_protocol::Kind::Ack &&
     frame.opcode==link_protocol::ui::kActivityMessage) {
    ui_.acknowledge(frame.transactionId);return;
  }
  MeasurementService::instance().handleFrame(frame);
  if (frame.service != link_protocol::Service::Application) return;
  if(frame.kind==link_protocol::Kind::Ack) {
    if(frame.opcode==link_protocol::application::kStop && frame.transactionId==0xFFFE)closingToken_=0;
    return;
  }
  if (frame.opcode == link_protocol::application::kList) { sendManifestList(frame.transactionId); return; }
  if (!frame.payloadSize || frame.payload[0] != 0) return;
  if (frame.opcode == link_protocol::application::kStart && frame.payloadSize >= 3) {
    const uint16_t id = link_protocol::readU16(frame.payload + 1);
    App* next = find(id); if (!next) return;
    if (active_) active_->onStop();
    closingToken_=0;ui_.reset();
    active_ = next; activityToken_ = static_cast<uint32_t>(random(1, 0x7fffffff));
    uint8_t accepted[7]{0, uint8_t(id), uint8_t(id >> 8), uint8_t(activityToken_),
      uint8_t(activityToken_ >> 8), uint8_t(activityToken_ >> 16), uint8_t(activityToken_ >> 24)};
    Link::instance().send(link_protocol::Kind::Response, link_protocol::Service::Application,
                          link_protocol::application::kStart, accepted, sizeof(accepted), frame.transactionId);
    active_->onStart(ui_, activityToken_);
  } else if (frame.opcode == link_protocol::application::kStop) {
    if (active_) active_->onStop(); active_ = nullptr; activityToken_ = 0;
    closingToken_=0;ui_.reset();
  } else if (frame.opcode == link_protocol::application::kInput && frame.payloadSize >= 7 &&
             active_ && link_protocol::readU32(frame.payload + 1) == activityToken_) {
    active_->onInput(static_cast<link_protocol::Key>(frame.payload[5]),
                     static_cast<link_protocol::KeyAction>(frame.payload[6]));
  }
}

void AppRuntime::closeActive() {
  if(!active_)return;
  closingToken_=activityToken_;closeSentMs_=millis()-500;ui_.reset();
  active_->onStop();active_=nullptr;activityToken_=0;
}

}  // namespace openscope
