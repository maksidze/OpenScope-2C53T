#pragma once
#include <cstddef>
#include <cstring>

namespace openscope {
// The editor owns only a bounded draft. Saving credentials is an explicit
// action of the Wi-Fi app; cancelling or changing keyboard pages never saves.
class WifiEditor {
 public:
  enum class Result { Changed, Submit, Cancel, Full };
  void begin(const char* initial, size_t maximum, bool secret) {
    maximum_=maximum>64?64:maximum;secret_=secret;visible_=!secret;
    page_=0;selected_=0;clear();
    if(initial)while(initial[length_] && length_<maximum_) { text_[length_]=initial[length_];++length_; }
    text_[length_]=0;
  }
  void move(int dx,int dy) {
    const int row=(int(selected_/10)+dy+5)%5;
    const int col=(int(selected_%10)+dx+10)%10;
    selected_=static_cast<unsigned>(row*10+col);
  }
  Result press() {
    if(selected_<40) {
      if(length_==maximum_)return Result::Full;
      text_[length_++]=pageChars()[selected_];text_[length_]=0;return Result::Changed;
    }
    switch(selected_-40) {
      case 0: page_=0;break;
      case 1: page_=1;break;
      case 2: page_=2;break;
      case 3:
        if(length_==maximum_)return Result::Full;
        text_[length_++]=' ';text_[length_]=0;break;
      case 4: if(length_)text_[--length_]=0;break;
      case 5: clear();break;
      case 6: visible_=!secret_;break;
      case 7: visible_=true;break;
      case 8: return Result::Submit;
      case 9: return Result::Cancel;
    }
    return Result::Changed;
  }
  void backspace() { if(length_)text_[--length_]=0; }
  void clear() { std::memset(text_,0,sizeof(text_));length_=0; }
  const char* text() const { return text_; }
  size_t length() const { return length_; }
  unsigned selected() const { return selected_; }
  unsigned page() const { return page_; }
  bool visible() const { return visible_; }
  char character(unsigned index) const { return pageChars()[index]; }
 private:
  const char* pageChars() const {
    static constexpr char lower[]="1234567890qwertyuiopasdfghjkl-zxcvbnm._@";
    static constexpr char upper[]="1234567890QWERTYUIOPASDFGHJKL-ZXCVBNM._@";
    static constexpr char symbols[]="!@#$%^&*()-_=+[]{}\\|;:'\",.<>/?`~01234567";
    static_assert(sizeof(lower)==41 && sizeof(upper)==41 && sizeof(symbols)==41,"Keyboard must be 4x10");
    return page_==0?lower:(page_==1?upper:symbols);
  }
  char text_[65]{};
  size_t length_=0,maximum_=63;
  unsigned page_=0,selected_=0;
  bool secret_=false,visible_=true;
};
}  // namespace openscope
