#include "instrument_web.hpp"
#include "dashboard_page.hpp"
#include "measurement_service.hpp"
#include "telemetry_json.hpp"
#include "app_runtime.hpp"

namespace openscope {
void InstrumentWeb::begin(WebServer& server){
  static char json[3072]; // ESP .bss, not the AT32 heap or Arduino loop stack.
  const char* headers[]{"X-OpenScope-Control","Origin"};server.collectHeaders(headers,2);
  server.on("/",HTTP_GET,[&server](){
    server.sendHeader("Cache-Control","no-store",true);
    server.send_P(200,"text/html; charset=utf-8",kDashboardPage);
  });
  server.on("/api/live",HTTP_GET,[&server](){
    auto& service=MeasurementService::instance();service.touchWeb();
    auto& link=Link::instance();TelemetryJson writer(json,sizeof(json));
    server.sendHeader("Cache-Control","no-store",true);
    if(!writer.render(service,link.connected(),link.sessionId())){
      server.send(500,"application/json","{\"error\":\"snapshot too large\"}");return;
    }
    server.send(200,"application/json",json);
  });
  server.on("/api/mode",HTTP_POST,[&server](){
    // Custom header requires a CORS preflight for cross-origin scripts. We
    // deliberately expose no CORS grants. This is CSRF protection, NOT login:
    // any client on this trusted LAN can explicitly control the instrument.
    const String origin=server.header("Origin");
    if(server.header("X-OpenScope-Control")!="1" ||
       (!origin.isEmpty() && origin!=String("http://")+server.hostHeader())){
      server.send(403,"application/json","{\"error\":\"same-origin control required\"}");return;
    }
    String mode=server.arg("mode");
    if(mode.length()!=1 || mode.c_str()[0]<'0' || mode.c_str()[0]>'3'){
      server.send(400,"application/json","{\"error\":\"mode must be 0..3\"}");return;
    }
    auto& service=MeasurementService::instance();
    if(!Link::instance().connected()){
      server.send(503,"application/json","{\"error\":\"AT32 offline\"}");return;
    }
    if(!service.requestMode(uint8_t(mode.c_str()[0]-'0'))){
      server.send(409,"application/json","{\"error\":\"mode transition pending\"}");return;
    }
    // Stop the foreground Activity at its owner as well. Its token-bound
    // close exchange retries independently, so one lost host STOP cannot leave
    // a Wi-Fi screen generating unacknowledged UI updates in a native mode.
    AppRuntime::instance().closeActive();
    server.send(202,"application/json","{\"queued\":true}");
  });
}
}
