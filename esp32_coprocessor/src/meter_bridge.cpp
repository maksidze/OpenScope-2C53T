#include "meter_bridge.hpp"

#include <cstring>
#include <driver/uart.h>
#include "config.hpp"
#include "link.hpp"

namespace openscope {
MeterBridge& MeterBridge::instance() { static MeterBridge bridge; return bridge; }

bool MeterBridge::begin() {
  if (task_) return true;
  controlRx_ = xQueueCreate(4, sizeof(Packet));
  controlTx_ = xQueueCreate(4, sizeof(Packet));
  if (!controlRx_ || !controlTx_) {
    if (controlRx_) vQueueDelete(controlRx_);
    if (controlTx_) vQueueDelete(controlTx_);
    controlRx_ = controlTx_ = nullptr;
    return false;
  }
  // ESP32-C3 has two hardware UART controllers; native USB CDC remains the
  // console. UART0 is deliberately remapped away from its ROM GPIO20/21 pins.
  // Those pins carry MCU USART2 after ROM boot, never diagnostic Serial text.
  meter_.setRxBufferSize(4096); meter_.setTxBufferSize(256);
  meter_.begin(config::kMeterUartBaud, SERIAL_8N1,
               config::kMeterUartRxGpio, config::kMeterUartTxGpio);
  host_.setRxBufferSize(4096); host_.setTxBufferSize(2048);
  host_.begin(config::kScopeUartBaud, SERIAL_8N1,
              config::kScopeUartRxGpio, config::kScopeUartTxGpio);
  if (xTaskCreate(taskEntry, "meter-bridge", 4096, this, 3, &task_) != pdPASS) {
    host_.end();meter_.end();
    vQueueDelete(controlRx_);vQueueDelete(controlTx_);
    controlRx_=controlTx_=nullptr;
    return false;
  }
  return true;
}

bool MeterBridge::receiveControl(Packet& packet) {
  return controlRx_ && xQueueReceive(controlRx_, &packet, 0)==pdTRUE;
}

bool MeterBridge::sendControl(const uint8_t* encoded, size_t length) {
  if (!controlTx_ || !encoded || !length || length>sizeof(Packet::data)) return false;
  Packet packet{};
  packet.length=static_cast<uint16_t>(length);
  memcpy(packet.data,encoded,length);
  if (xQueueSend(controlTx_,&packet,0)==pdTRUE) return true;
  ++controlDrops_;return false;
}

void MeterBridge::taskEntry(void* context) { static_cast<MeterBridge*>(context)->run(); }

bool MeterBridge::sendMeterFrame(link_protocol::Kind kind, uint8_t opcode,
                                  uint16_t transaction,const uint8_t* data,size_t length) {
  if (length>64) return false;
  uint8_t raw[84]{};
  raw[0]='O';raw[1]='S';raw[2]=link_protocol::kProtocolMajor;
  raw[4]=static_cast<uint8_t>(kind);raw[5]=link_protocol::PayloadRaw;
  raw[6]=static_cast<uint8_t>(link_protocol::Service::MeterBridge);raw[7]=opcode;
  raw[10]=transaction;raw[11]=transaction>>8;
  raw[14]=static_cast<uint8_t>(length);
  if(length)memcpy(raw+16,data,length);
  uint32_t crc=Link::crc32c(raw,16+length);
  for(unsigned i=0;i<4;i++)raw[16+length+i]=crc>>(8*i);
  size_t n=Link::cobsEncode(raw,20+length,txWire_,sizeof(txWire_)-1);
  if(!n){++errors_;return false;}
  txWire_[n++]=0;
  if(host_.write(txWire_,n)==n)return true;
  ++errors_;return false;
}

void MeterBridge::dispatch(size_t length) {
  // Validate the complete OSLP envelope BEFORE routing anything downstream.
  // The original 10-byte AA55 meter command (including its checksum and zero
  // bytes) is opaque here. Changing it would break transparent forwarding.
  const uint8_t* p=decoded_.data;
  if(length<20 || p[0]!='O' || p[1]!='S' || p[2]!=1 ||
     link_protocol::readU16(p+14)+20u!=length ||
     Link::crc32c(p,length-4)!=link_protocol::readU32(p+length-4)) {
    ++errors_;return;
  }
  if(p[6]==static_cast<uint8_t>(link_protocol::Service::MeterBridge)) {
    uint16_t transaction=link_protocol::readU16(p+10);
    if(p[7]!=link_protocol::meter_bridge::kTx ||
       p[4]!=static_cast<uint8_t>(link_protocol::Kind::Request) || length!=30) {
      sendMeterFrame(link_protocol::Kind::Error,link_protocol::meter_bridge::kTxDone,
                     transaction,nullptr,0);return;
    }
    size_t n=meter_.write(p+16,10);
    meter_.flush(); // Wait for the final 9600-baud stop bit, not just enqueue.
    meterTxBytes_+=static_cast<uint32_t>(n);
    sendMeterFrame(n==10?link_protocol::Kind::Ack:link_protocol::Kind::Error,
                    link_protocol::meter_bridge::kTxDone,transaction,nullptr,0);
    return;
  }
  decoded_.length=static_cast<uint16_t>(length);
  decoded_.wireLength=static_cast<uint16_t>(used_+1);
  // Control overload can drop a UI update, never block meter traffic.
  if(xQueueSend(controlRx_,&decoded_,0)!=pdTRUE)++controlDrops_;
}

void MeterBridge::receiveHost() {
  // Limit each pass so even a continuously transmitting host cannot starve
  // the slow UART's RX drain or the outgoing meter stream.
  for(unsigned budget=0;budget<1024 && host_.available();budget++) {
    int value=host_.read();if(value<0)break;
    uint8_t b=static_cast<uint8_t>(value);lastByteMs_=millis();
    if(b) {
      if(!discard_ && used_<sizeof(incoming_.data))incoming_.data[used_++]=b;
      else discard_=true;
    } else {
      if(!discard_ && used_) {
        size_t n=Link::cobsDecode(incoming_.data,used_,decoded_.data,sizeof(decoded_.data));
        if(n)dispatch(n);else ++errors_;
      } else if(discard_)++errors_;
      used_=0;discard_=false;
    }
  }
  if(used_ && static_cast<uint32_t>(millis()-lastByteMs_)>200) {
    used_=0;discard_=true;++errors_;
  }
}

void MeterBridge::run() {
  for(;;) {
    receiveHost();
    // At most one control frame may already be on the wire. Meter RX and
    // TX_DONE are inserted before the next UI/application envelope. A native
    // 1024-byte envelope takes <90 ms at 115200; the MCU timeout is 150 ms.
    uint8_t bytes[64];size_t used=0;
    while(used<sizeof(bytes) && meter_.available()) {
      int value=meter_.read();if(value<0)break;bytes[used++]=static_cast<uint8_t>(value);
    }
    if(used) {
      meterRxBytes_+=static_cast<uint32_t>(used);
      sendMeterFrame(link_protocol::Kind::Stream,link_protocol::meter_bridge::kRx,0,bytes,used);
    }
    if(uart_wait_tx_done(UART_NUM_1,0)==ESP_OK &&
       xQueueReceive(controlTx_,&outgoing_,0)==pdTRUE)
      host_.write(outgoing_.data,outgoing_.length);
    vTaskDelay(1);
  }
}
}  // namespace openscope
