#pragma once
#include <cstddef>
#include <cstring>

namespace openscope {
inline bool validWifiSsid(const char* ssid) {
  return ssid && std::strlen(ssid)>0 && std::strlen(ssid)<=32;
}
// WPA passphrases are 8..63 printable ASCII bytes. Empty means an open
// station network; a 64-byte station key must contain exactly 64 hex digits.
inline bool validWifiPassword(const char* password, bool allowOpen, bool allowPsk) {
  if(!password)return false;
  const size_t n=std::strlen(password);
  if(!n)return allowOpen;
  if(n==64 && allowPsk) {
    for(size_t i=0;i<n;i++)if(!((password[i]>='0'&&password[i]<='9') ||
      (password[i]>='a'&&password[i]<='f') || (password[i]>='A'&&password[i]<='F')))return false;
    return true;
  }
  if(n<8 || n>63)return false;
  for(size_t i=0;i<n;i++)if(static_cast<unsigned char>(password[i])<32 ||
                            static_cast<unsigned char>(password[i])>126)return false;
  return true;
}
}  // namespace openscope
