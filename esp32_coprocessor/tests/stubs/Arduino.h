#pragma once
#include <cstddef>
#include <cstdint>
#include <vector>
#include <deque>
#include <string>
#include <cstring>
#include <cstdio>
#define F(text) text
#define PROGMEM
class String {
 public:
  String()=default;
  String(const char* s):value_(s?s:""){}
  String(const std::string& s):value_(s){}
  String(int n):value_(std::to_string(n)){}
  String(unsigned n):value_(std::to_string(n)){}
  String(long n):value_(std::to_string(n)){}
  String(unsigned long n):value_(std::to_string(n)){}
  String(long long n):value_(std::to_string(n)){}
  String(unsigned long long n):value_(std::to_string(n)){}
  const char* c_str() const{return value_.c_str();}
  bool isEmpty() const{return value_.empty();}
  size_t length() const{return value_.size();}
  long toInt() const{return value_.empty()?0:std::stol(value_);}
  String& operator+=(const String& s){value_+=s.value_;return *this;}
  friend String operator+(const String& a,const String& b){return a.value_+b.value_;}
  friend bool operator==(const String& a,const String& b){return a.value_==b.value_;}
  friend bool operator!=(const String& a,const String& b){return !(a==b);}
 private:std::string value_;
};
struct MockEsp { uint64_t getEfuseMac() const{return 0x1234;} };
inline MockEsp ESP;

inline uint32_t mockMs;
inline uint32_t millis() { return mockMs; }
inline long random(long lower, long) { return lower; }
constexpr int SERIAL_8N1=0;
inline std::vector<int> mockEvents;

class HardwareSerial {
 public:
  explicit HardwareSerial(unsigned port) : port_(port) {}
  void setRxBufferSize(size_t) {}
  void setTxBufferSize(size_t) {}
  void begin(uint32_t, int, int, int) {}
  void end() {}
  int available() const { return static_cast<int>(rx.size()); }
  int read() { if(rx.empty())return -1;int b=rx.front();rx.pop_front();return b; }
  size_t write(const uint8_t* p,size_t n) {
    mockEvents.push_back(static_cast<int>(port_));
    tx.insert(tx.end(),p,p+n);return n;
  }
  void flush() { mockEvents.push_back(100+static_cast<int>(port_)); }
  std::deque<uint8_t> rx;
  std::vector<uint8_t> tx;
 private:
  unsigned port_;
};
