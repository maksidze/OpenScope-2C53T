#include "../apps.hpp"
#include "../wifi_manager.hpp"
#include "../wifi_editor.hpp"
#include "../wifi_validation.hpp"
#include <algorithm>

namespace openscope {
namespace {
class WifiSettingsApp final : public App {
 public:
  const AppManifest& manifest() const override { return manifest_; }
  void onStart(UiClient& ui,uint32_t token) override { ui_=&ui;token_=token;notice_="";dashboard(); }
  void onStop() override { editor_.clear();staPass_="";apPass_="";ui_=nullptr;token_=0; }
  void onInput(link_protocol::Key key,link_protocol::KeyAction action) override {
    using K=link_protocol::Key;
    if(!ui_ || action!=link_protocol::KeyAction::Down)return;
    if(page_==Page::Editor){editorInput(key);return;}
    if(key==K::Menu) {
      if(page_==Page::Dashboard)AppRuntime::instance().closeActive();else dashboard();
      return;
    }
    if(page_==Page::Networks){networkInput(key);return;}
    const unsigned count=page_==Page::Dashboard?6:page_==Page::Ap?5:page_==Page::Confirm?3:2;
    if(key==K::Up || key==K::Down) {
      selection_=(selection_+count+(key==K::Up?-1:1))%count;ui_->select(token_,10,selection_);return;
    }
    if(page_==Page::Ap && selection_==2 && (key==K::Left || key==K::Right)) {
      channel_=uint8_t((channel_-1+13+(key==K::Left?-1:1))%13+1);renderAp();return;
    }
    if(key!=K::Ok)return;
    auto& wifi=WifiManager::instance();
    if(page_==Page::Dashboard) {
      switch(selection_) {
        case 0:scan();break;
        case 1:staSsid_="";staPass_="";open_=true;edit(Field::StaSsid,"",32,false);break;
        case 2:notice_=wifi.setMode(WifiMode::Station)?"Connecting to saved network...":"No saved network / storage error";dashboard();break;
        case 3:{const auto s=wifi.status();apSsid_=s.apSsid;apPass_=wifi.apPassword();channel_=s.apChannel;selection_=0;renderAp();break;}
        case 4:notice_=wifi.setMode(WifiMode::AccessPoint)?"Enabling saved access point...":"Cannot save mode";dashboard();break;
        case 5:selection_=0;renderForget();break;
      }
    } else if(page_==Page::Ap) {
      if(selection_==0)edit(Field::ApSsid,apSsid_.c_str(),32,false);
      else if(selection_==1)edit(Field::ApPass,apPass_.c_str(),63,true);
      else if(selection_==2){channel_=channel_%13+1;renderAp();}
      else if(selection_==3) {
        if(wifi.configureAccessPoint(apSsid_,apPass_,channel_)){notice_="AP settings saved in ESP32";apPass_="";dashboard();}
        else ui_->setText(token_,2,"Invalid AP settings / NVS save failed");
      } else dashboard();
    } else if(page_==Page::Confirm) {
      if(selection_==0) {
        if(wifi.configureStation(staSsid_,staPass_)){notice_="Saved in ESP32. Connecting...";staPass_="";editor_.clear();dashboard();}
        else ui_->setText(token_,2,"Unable to save Wi-Fi settings (NVS)");
      } else if(selection_==1)edit(Field::StaPass,staPass_.c_str(),64,true);
      else {staPass_="";dashboard();}
    } else if(page_==Page::Forget) {
      notice_=selection_==0?(wifi.forgetStation()?"Saved network removed; enabling AP":"Could not remove saved settings"):"";dashboard();
    }
  }
  void loop() override {
    if(!ui_)return;
    if(page_==Page::Networks && scanRevision_!=WifiManager::instance().scanRevision()) {
      scanRevision_=WifiManager::instance().scanRevision();selection_=0;renderNetworks();
    }
    if(page_==Page::Dashboard && static_cast<uint32_t>(millis()-updatedMs_)>=1000)updateStatus();
  }
 private:
  enum class Page { Dashboard,Networks,Editor,Confirm,Ap,Forget };
  enum class Field { StaSsid,StaPass,ApSsid,ApPass };
  void start(Page page,const char* title){page_=page;ui_->begin(token_,manifest_.id,title);}
  void label(uint16_t id,int y,int height,const char* text){ui_->label(token_,id,5,y,310,height,1,text);}
  void menu(const char* const* items,unsigned count,int y,int height) {
    ui_->grid(token_,10,4,y,312,height,1,items,count);ui_->select(token_,10,selection_);
  }
  void footer(const char* text="Arrows: move   OK: select   MENU: back"){label(3,211,29,text);ui_->ready(token_);}
  void dashboard() {
    editor_.clear();staPass_="";apPass_="";
    selection_=0;noticeMs_=millis();start(Page::Dashboard,"Wi-Fi settings");label(1,23,42,"");label(2,66,43,"");
    static const char* items[]{"Scan Wi-Fi networks","Enter SSID manually","Connect saved Wi-Fi","Configure access point","Use access point mode","Forget saved Wi-Fi"};
    menu(items,6,111,96);footer();cachedState_="";cachedDetail_="";updateStatus();
  }
  void updateStatus() {
    updatedMs_=millis();const auto s=WifiManager::instance().status();String state;
    if(s.connected || static_cast<uint32_t>(millis()-noticeMs_)>=4000)notice_="";
    switch(s.connection) {
      case WifiConnection::AccessPoint:state="AP active (not an Internet connection)";break;
      case WifiConnection::Applying:state="Applying saved settings...";break;
      case WifiConnection::Connected:state="Connected to router  "+String(s.rssi)+" dBm";break;
      case WifiConnection::Connecting:state="Connecting / waiting for IP. Try "+String(s.attempts);break;
      case WifiConnection::NetworkNotFound:state="Network not found. Try "+String(s.attempts);break;
      case WifiConnection::AuthenticationFailed:state="Authentication failed: check password";break;
      default:state=s.configuredMode==WifiMode::AccessPoint?String("AP failed to start. Check settings"):String("Disconnected. Reason ")+String(s.disconnectReason);break;
    }
    state+="\n";state+=s.configuredMode==WifiMode::Station?("SSID: "+s.stationSsid):("AP: "+s.apSsid);
    String detail="IP: "+s.ip;
    if(s.apRunning)detail+="  AP clients: "+String(s.apClients);
    if(s.fallbackAp)detail+="\nFallback AP: "+s.apSsid+" "+s.apIp;
    else if(!notice_.isEmpty())detail+="\n"+notice_;
    else detail+="\n"+(s.connected?String("Router connected; Internet not checked"):String("2.4 GHz Wi-Fi / settings saved in ESP32"));
    if(state!=cachedState_){ui_->setText(token_,1,state.c_str());cachedState_=state;}
    if(detail!=cachedDetail_){ui_->setText(token_,2,detail.c_str());cachedDetail_=detail;}
  }
  void scan(){selection_=0;auto& w=WifiManager::instance();w.startScan();scanRevision_=w.scanRevision();renderNetworks();}
  void renderNetworks() {
    auto& w=WifiManager::instance();start(Page::Networks,"Nearby Wi-Fi (2.4 GHz)");
    if(w.scanning()){label(1,45,65,"Scanning...\nPlease wait (AUTO: rescan, MENU: back)");footer();return;}
    const size_t count=w.networkCount();
    if(!count){label(1,40,100,w.scanFailed()?"Scan failed. AUTO: retry":"No visible networks. AUTO: rescan\nUse manual SSID for hidden networks.");footer();return;}
    if(selection_>=count)selection_=0;
    const size_t first=(selection_/5)*5,shown=std::min<size_t>(5,count-first);
    char heading[64];snprintf(heading,sizeof(heading),"Networks %u-%u / %u   [O] open [L] locked",unsigned(first+1),unsigned(first+shown),unsigned(count));label(1,27,40,heading);
    char names[5][61];const char* items[5];
    for(size_t i=0;i<shown;i++) {
      const auto& n=w.network(first+i);
      snprintf(names[i],sizeof(names[i]),"%s %s (%ld dBm)",n.open?"[O]":"[L]",n.ssid.c_str(),long(n.rssi));
      // SSIDs are untrusted radio data. A literal newline is an LVGL map row
      // separator, not a caption; sanitize only the view, never the real SSID.
      for(char* p=names[i];*p;p++)if(static_cast<unsigned char>(*p)<32 || *p==127)*p='?';
      items[i]=names[i];
    }
    ui_->grid(token_,10,4,71,312,131,1,items,shown);ui_->select(token_,10,selection_-first);
    footer("UP/DOWN: network  OK: join\nAUTO: rescan   MENU: back");
  }
  void networkInput(link_protocol::Key key) {
    using K=link_protocol::Key;auto& w=WifiManager::instance();if(key==K::Auto){scan();return;}
    const size_t count=w.networkCount();if(w.scanning() || !count)return;
    if(key==K::Up || key==K::Down || key==K::Left || key==K::Right) {
      const unsigned oldPage=selection_/5;
      int next=int(selection_)+(key==K::Up?-1:key==K::Down?1:key==K::Left?-5:5);
      if(next<0)next=int(count)-1;
      if(next>=int(count))next=0;
      selection_=next;
      if(oldPage!=selection_/5)renderNetworks();else ui_->select(token_,10,selection_%5);
    } else if(key==K::Ok) {
      const auto& n=w.network(selection_);staSsid_=n.ssid;open_=n.open;staPass_="";
      if(open_){selection_=0;renderConfirm();}else edit(Field::StaPass,"",64,true);
    }
  }
  void edit(Field field,const char* initial,size_t maximum,bool secret){field_=field;editor_.begin(initial,maximum,secret);renderEditor();}
  void renderEditor() {
    const char* title=field_==Field::StaSsid?"Network name (SSID)":field_==Field::StaPass?"Wi-Fi password":field_==Field::ApSsid?"Access point name":"Access point password";
    start(Page::Editor,title);label(1,25,38,"");label(2,64,21,"SAVE: done   PRM: delete last character");
    char chars[40][2];const char* keys[50];
    for(unsigned i=0;i<40;i++){chars[i][0]=editor_.character(i);chars[i][1]=0;keys[i]=chars[i];}
    static const char* controls[]{"abc","ABC","#+=","SP","Del","Clr","Hid","Vis","OK","Bk"};
    for(unsigned i=0;i<10;i++)keys[40+i]=controls[i];
    ui_->grid(token_,10,3,87,314,118,10,keys,50);ui_->select(token_,10,editor_.selected());
    footer("Arrows: move   OK: type / action\nMENU: cancel (nothing is saved)");editorText();
  }
  void editorText() {
    char value[96];
    if(editor_.visible())snprintf(value,sizeof(value),"%s_  (%u)",editor_.text(),unsigned(editor_.length()));
    else {memset(value,'*',editor_.length());snprintf(value+editor_.length(),sizeof(value)-editor_.length(),"_  (%u)",unsigned(editor_.length()));}
    ui_->setText(token_,1,value);
  }
  void cancelEditor(){editor_.clear();if(field_==Field::ApSsid || field_==Field::ApPass)renderAp();else {staPass_="";dashboard();}}
  void submitEditor() {
    const bool isSsid=field_==Field::StaSsid || field_==Field::ApSsid;
    if(isSsid && !validWifiSsid(editor_.text())){ui_->setText(token_,2,"SSID must contain 1-32 bytes");return;}
    if(!isSsid && (!validWifiPassword(editor_.text(),field_==Field::StaPass,field_==Field::StaPass) || (field_==Field::StaPass && !open_ && !editor_.length()))) {
      ui_->setText(token_,2,"Use 8-63 ASCII characters (STA: 64 hex)");return;
    }
    switch(field_) {
      case Field::StaSsid:staSsid_=editor_.text();open_=true;edit(Field::StaPass,"",64,true);return;
      case Field::StaPass:staPass_=editor_.text();selection_=0;editor_.clear();renderConfirm();break;
      case Field::ApSsid:apSsid_=editor_.text();editor_.clear();renderAp();break;
      case Field::ApPass:apPass_=editor_.text();editor_.clear();renderAp();break;
    }
  }
  void editorInput(link_protocol::Key key) {
    using K=link_protocol::Key;
    if(key==K::Menu){cancelEditor();return;}
    if(key==K::Save){submitEditor();return;}
    if(key==K::Prm){editor_.backspace();editorText();return;}
    if(key==K::Up || key==K::Down || key==K::Left || key==K::Right) {
      editor_.move(key==K::Left?-1:key==K::Right?1:0,key==K::Up?-1:key==K::Down?1:0);ui_->select(token_,10,editor_.selected());return;
    }
    if(key!=K::Ok)return;
    const unsigned oldPage=editor_.page();const auto result=editor_.press();
    if(result==WifiEditor::Result::Submit)submitEditor();
    else if(result==WifiEditor::Result::Cancel)cancelEditor();
    else if(result==WifiEditor::Result::Full)ui_->setText(token_,2,"Maximum length reached. PRM: delete");
    else if(editor_.page()!=oldPage)renderEditor();else editorText();
  }
  void renderConfirm() {
    start(Page::Confirm,"Save and connect?");
    const String text="SSID: "+staSsid_+"\n"+(staPass_.isEmpty()?String("OPEN network - no encryption"):String("Password: hidden (")+String(staPass_.length())+" chars)");
    label(1,30,58,text.c_str());label(2,89,30,"Saved in ESP32, including after power-off");
    static const char* items[]{"Save and connect","Edit password","Cancel (do not save)"};menu(items,3,122,80);footer();
  }
  void renderAp() {
    start(Page::Ap,"Access point settings");
    const String info="AP: "+apSsid_+"\nPassword: hidden ("+String(apPass_.length())+" chars)";
    label(1,26,47,info.c_str());label(2,75,22,"Edits saved only by Save and enable AP");
    char channel[48];snprintf(channel,sizeof(channel),"Channel: %u (LEFT/RIGHT)",channel_);
    const char* items[]{"Edit AP name","Edit AP password",channel,"Save and enable AP","Cancel"};menu(items,5,99,108);footer();
  }
  void renderForget() {
    start(Page::Forget,"Forget saved Wi-Fi?");label(1,40,54,"Removes the saved router SSID/password.\nThe device will use access point mode.");
    static const char* items[]{"Forget and enable AP","Cancel"};menu(items,2,115,78);footer();
  }
  const AppManifest manifest_{1,"Wi-Fi settings","0.2.0","Scan, connect, keyboard and saved AP/STA settings"};
  Page page_=Page::Dashboard;Field field_=Field::StaSsid;WifiEditor editor_;
  String staSsid_,staPass_,apSsid_,apPass_,notice_,cachedState_,cachedDetail_;
  unsigned selection_=0;uint8_t channel_=1;bool open_=false;
  uint32_t updatedMs_=0,scanRevision_=0,noticeMs_=0;
};
}
App& wifiSettingsApp(){static WifiSettingsApp app;return app;}
}
