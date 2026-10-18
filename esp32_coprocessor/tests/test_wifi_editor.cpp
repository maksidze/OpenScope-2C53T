#include "../src/wifi_editor.hpp"
#include "../src/wifi_validation.hpp"
#include <cstdio>
#include <cstdlib>
#include <string>
using namespace openscope;
static unsigned checks;
#define CHECK(x) do {++checks;if(!(x)){std::fprintf(stderr,"FAIL line %d: %s\n",__LINE__,#x);std::exit(1);}}while(0)
static void select(WifiEditor& e,unsigned index) {
  e.move(int(index%10)-int(e.selected()%10),int(index/10)-int(e.selected()/10));
  CHECK(e.selected()==index);
}
int main() {
  WifiEditor e;e.begin("",64,true);CHECK(!e.visible());
  e.move(-1,-1);CHECK(e.selected()==49);CHECK(e.press()==WifiEditor::Result::Cancel);
  bool reachable[127]{};reachable[' ']=true;
  for(unsigned page=0;page<3;page++) {
    select(e,40+page);CHECK(e.press()==WifiEditor::Result::Changed);CHECK(e.page()==page);
    for(unsigned i=0;i<40;i++) {
      unsigned char c=e.character(i);CHECK(c>=33 && c<=126);reachable[c]=true;
      select(e,i);e.clear();CHECK(e.press()==WifiEditor::Result::Changed);
      CHECK(e.length()==1 && e.text()[0]==c);
    }
  }
  for(unsigned c=32;c<=126;c++)CHECK(reachable[c]);
  e.begin("12345678",8,true);select(e,0);CHECK(e.press()==WifiEditor::Result::Full);
  CHECK(e.length()==8);e.backspace();CHECK(e.length()==7);
  CHECK(e.press()==WifiEditor::Result::Changed);CHECK(e.length()==8);
  select(e,47);e.press();CHECK(e.visible());select(e,46);e.press();CHECK(!e.visible());
  select(e,48);CHECK(e.press()==WifiEditor::Result::Submit);
  select(e,45);e.press();CHECK(!e.length() && !e.text()[0]);
  select(e,43);e.press();CHECK(e.length()==1 && e.text()[0]==' ');
  select(e,44);e.press();CHECK(e.length()==0);e.backspace();CHECK(e.length()==0);
  const std::string key64(64,'a'),longKey(65,'a'),ssid32(32,'s');
  e.begin(longKey.c_str(),100,true);CHECK(e.length()==64 && e.text()[64]==0);
  e.begin("router",32,false);CHECK(e.visible() && e.length()==6 && e.page()==0);
  CHECK(validWifiSsid(ssid32.c_str()));CHECK(!validWifiSsid(""));CHECK(!validWifiSsid((ssid32+"s").c_str()));CHECK(!validWifiSsid(nullptr));
  CHECK(validWifiPassword("",true,true));CHECK(!validWifiPassword("",false,false));
  CHECK(!validWifiPassword("1234567",true,true));CHECK(validWifiPassword("12345678",false,false));
  CHECK(validWifiPassword(std::string(63,'~').c_str(),false,false));
  CHECK(validWifiPassword(key64.c_str(),true,true));CHECK(!validWifiPassword(key64.c_str(),false,false));
  CHECK(!validWifiPassword(std::string(64,'g').c_str(),true,true));
  CHECK(!validWifiPassword(longKey.c_str(),true,true));CHECK(!validWifiPassword("1234567\n",true,true));
  CHECK(!validWifiPassword(nullptr,true,true));
  std::printf("Wi-Fi editor/validation: %u checks passed (all 95 printable ASCII characters)\n",checks);
}
