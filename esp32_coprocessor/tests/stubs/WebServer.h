#pragma once
#include "Arduino.h"
#include <map>
#include <functional>
enum {HTTP_GET,HTTP_POST};
class WebServer {
 public:
  explicit WebServer(int){}
  template<typename Callback>void on(const char* path,int method,Callback callback){routes[std::to_string(method)+path]=callback;}
  template<typename Callback>void onNotFound(Callback){}
  void begin(){}
  void handleClient(){}
  void send(int code,const char*,const String& body){responseCode=code;responseBody=body.c_str();}
  void send_P(int code,const char* type,const char* body){send(code,type,body);}
  void send(int code){responseCode=code;}
  void sendHeader(const char*,const char*,bool){}
  String arg(const char* key){return args[key];}
  void collectHeaders(const char*[],size_t){}
  String header(const char* key){return headers[key];}
  String hostHeader(){return "192.168.1.200";}
  void invoke(const char* path,int method){routes.at(std::to_string(method)+path)();}
  std::map<std::string,std::function<void()>> routes;
  std::map<std::string,String> args,headers;
  int responseCode=0;
  std::string responseBody;
};
