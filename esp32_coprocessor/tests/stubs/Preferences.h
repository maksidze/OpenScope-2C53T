#pragma once
#include "Arduino.h"
#include <map>
inline std::map<std::string,std::vector<uint8_t>> mockNvs;
inline bool mockNvsFailure=false;
class Preferences {
 public:
  bool begin(const char*,bool){return !mockNvsFailure;}
  String getString(const char* key,const String& fallback) {
    auto it=mockNvs.find(key);if(it==mockNvs.end())return fallback;
    return std::string(it->second.begin(),it->second.end());
  }
  size_t putString(const char* key,const String& text){return putBytes(key,text.c_str(),text.length());}
  uint8_t getUChar(const char* key,uint8_t fallback){return isKey(key)?mockNvs[key][0]:fallback;}
  size_t putUChar(const char* key,uint8_t value){return putBytes(key,&value,1);}
  size_t getBytesLength(const char* key){return isKey(key)?mockNvs[key].size():0;}
  size_t getBytes(const char* key,void* data,size_t capacity) {
    if(!isKey(key) || mockNvs[key].size()>capacity)return 0;
    memcpy(data,mockNvs[key].data(),mockNvs[key].size());return mockNvs[key].size();
  }
  size_t putBytes(const char* key,const void* data,size_t length) {
    if(mockNvsFailure)return 0;
    const auto* p=static_cast<const uint8_t*>(data);mockNvs[key]={p,p+length};return length;
  }
  bool isKey(const char* key){return mockNvs.count(key)!=0;}
  bool remove(const char* key){if(mockNvsFailure)return false;return mockNvs.erase(key)!=0;}
};
