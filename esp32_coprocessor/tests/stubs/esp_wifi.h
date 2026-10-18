#pragma once
#include "WiFi.h"
inline void esp_wifi_scan_stop(){WiFi.scanResult=WIFI_SCAN_FAILED;}
