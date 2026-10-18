#pragma once
#include <WebServer.h>

namespace openscope {
class InstrumentWeb {
 public:
  static void begin(WebServer& server);
};
}
