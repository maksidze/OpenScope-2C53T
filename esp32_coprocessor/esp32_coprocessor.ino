#include "src/app_runtime.hpp"
#include "src/apps.hpp"
#include "src/config.hpp"
#include "src/link.hpp"
#include "src/wifi_manager.hpp"
#include "src/meter_bridge.hpp"
#include "src/instrument_web.hpp"

#if !ARDUINO_USB_CDC_ON_BOOT
#error "Enable Tools > USB CDC On Boot. Both hardware UARTs are used by the meter bridge."
#endif

void setup() {
  // Start forwarding before waiting for USB or starting Wi-Fi. The scope can
  // already be booting and issuing meter selectors while no PC is attached.
  const bool bridgeOk=openscope::MeterBridge::instance().begin();
  Serial.begin(115200);
  const uint32_t serialDeadline = millis() + 2000;
  while (!Serial && static_cast<int32_t>(millis() - serialDeadline) < 0) delay(10);
  Serial.println();
  Serial.println("[boot] OpenScope ESP32-C3 coprocessor");
  Serial.printf("[boot] meter bridge %s, meter RX=GPIO%d TX=GPIO%d baud=%lu\n",
                bridgeOk?"ready":"FAILED",openscope::config::kMeterUartRxGpio,
                openscope::config::kMeterUartTxGpio,
                static_cast<unsigned long>(openscope::config::kMeterUartBaud));
  Serial.printf("[boot] scope UART RX=GPIO%d TX=GPIO%d baud=%lu\n",
                openscope::config::kScopeUartRxGpio,
                openscope::config::kScopeUartTxGpio,
                static_cast<unsigned long>(openscope::config::kScopeUartBaud));
  randomSeed(static_cast<uint32_t>(ESP.getEfuseMac()) ^ micros());

  Serial.println("[boot] Wi-Fi manager...");
  openscope::WifiManager::instance().begin();
  Serial.println("[boot] application runtime...");
  auto& runtime = openscope::AppRuntime::instance();
  runtime.registerApp(&openscope::wifiSettingsApp());
  runtime.registerApp(&openscope::timeApp());
  runtime.registerApp(&openscope::hardwareDemoApp());
  runtime.begin();
  openscope::InstrumentWeb::begin(openscope::WifiManager::instance().server());

  Serial.println("[boot] started");
}

void loop() {
  openscope::WifiManager::instance().loop();
  openscope::AppRuntime::instance().loop();

  static uint32_t lastDiagnosticMs = 0;
  const uint32_t now = millis();
  if (static_cast<uint32_t>(now - lastDiagnosticMs) >= 1000) {
    lastDiagnosticMs = now;
    const auto wifi = openscope::WifiManager::instance().status();
    const auto& link = openscope::Link::instance();
    Serial.printf("[diag] up=%lus heap=%u wifi=%s ip=%s link=%s phy=%s session=%u "
                  "rx=%lu tx=%lu frames=%lu/%lu hello=%lu\n",
                  static_cast<unsigned long>(now / 1000),
                  static_cast<unsigned>(ESP.getFreeHeap()),
                  wifi.connected ? "connected" : (wifi.fallbackAp ? "fallback-ap" : "ap/offline"),
                  wifi.ip.c_str(), link.connected() ? "online" : "offline",
                  link.transmitQuiesced() ? "quiet" : "listen",
                  static_cast<unsigned>(link.sessionId()),
                  static_cast<unsigned long>(link.rxBytes()),
                  static_cast<unsigned long>(link.txBytes()),
                  static_cast<unsigned long>(link.validFrames()),
                  static_cast<unsigned long>(link.frameAttempts()),
                  static_cast<unsigned long>(link.helloCount()));
    const auto& bridge=openscope::MeterBridge::instance();
    Serial.printf("[bridge] meter rx=%lu tx=%lu errors=%lu control_drops=%lu\n",
                  static_cast<unsigned long>(bridge.meterRxBytes()),
                  static_cast<unsigned long>(bridge.meterTxBytes()),
                  static_cast<unsigned long>(bridge.errors()),
                  static_cast<unsigned long>(bridge.controlDrops()));
  }
  delay(1);
}
