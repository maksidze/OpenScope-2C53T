#include "../apps.hpp"
#include "../config.hpp"
#include "../measurement_service.hpp"

#include <cmath>
#include "soc/soc.h"
#if __has_include("soc/apb_saradc_reg.h")
#include "soc/apb_saradc_reg.h"
#endif

namespace openscope {
namespace {
class HardwareDemoApp final : public App {
 public:
  const AppManifest& manifest() const override { return manifest_; }
  void onStart(UiClient& ui, uint32_t token) override {
    ui_ = &ui; token_ = token; phase_ = 0; lastUpdateMs_ = 0;
    ui.begin(token_, manifest_.id, manifest_.name);
    ui.label(token_, 1, 8, 28, 304, 28, 2, "ESP supply: N/A");
    ui.label(token_, 2, 8, 58, 304, 28, 1, "Key: -");
    ui.label(token_, 3, 8, 86, 304, 24, 1, "AT32 measurements: OFF (PRM toggles)");
    ui.chart(token_, 4, 8, 116, 304, 96, 160);
    ui.label(token_, 5, 8, 214, 304, 20, 0, "VDD needs external divider -> GPIO0 ADC");
    ui.ready(token_); initAdc(); update();
  }
  void onStop() override { ui_ = nullptr; token_ = 0; }
  void onInput(link_protocol::Key key, link_protocol::KeyAction action) override {
    if (!ui_) return;
    char line[64]; snprintf(line, sizeof(line), "Key: %s / %s", keyName(key), actionName(action));
    ui_->setText(token_, 2, line);
    if (key == link_protocol::Key::Prm && action == link_protocol::KeyAction::Down) {
      auto& service = MeasurementService::instance(); service.setEnabled(!service.enabled());
      ui_->setText(token_, 3, service.enabled() ? "AT32 measurements: ON (background)"
                                                : "AT32 measurements: OFF (PRM toggles)");
    }
  }
  void loop() override {
    if (static_cast<uint32_t>(millis() - lastUpdateMs_) >= 100) update();
  }
 private:
  void initAdc() {
    // Requested register operation: clock gating only. It does not expose an
    // internal VDD channel; the external divider remains the hardware source.
#if defined(APB_SARADC_APB_ADC_CLKM_CONF_REG) && defined(APB_SARADC_CLK_EN)
    SET_PERI_REG_MASK(APB_SARADC_APB_ADC_CLKM_CONF_REG, APB_SARADC_CLK_EN);
#endif
    analogReadResolution(12);
    analogSetPinAttenuation(config::kSupplyAdcGpio, ADC_11db);
  }
  bool readSupply(float& volts) {
    uint32_t sum = 0;
    for (uint8_t i = 0; i < 16; ++i) sum += analogReadMilliVolts(config::kSupplyAdcGpio);
    const float pinMillivolts = sum / 16.0f;
    volts = pinMillivolts * config::kSupplyDividerRatio / 1000.0f;
    return pinMillivolts > 10 && pinMillivolts < 3050 && volts > 1.0f && volts < 6.0f;
  }
  void update() {
    if (!ui_) return;
    lastUpdateMs_ = millis(); float supply = 0;
    if (readSupply(supply)) ui_->setFloat(token_, 1, supply, "V");
    else ui_->setText(token_, 1, "ESP supply: N/A (check external divider)");
    int16_t samples[160];
    constexpr float pi = 3.14159265358979323846f;
    for (size_t i = 0; i < 160; ++i) samples[i] = int16_t(1000 * sinf(phase_ + 2 * pi * i / 80));
    phase_ += 0.12f; if (phase_ > 2 * pi) phase_ -= 2 * pi;
    ui_->setSeries(token_, 4, samples, 160);
  }
  static const char* keyName(link_protocol::Key key) {
    static const char* names[]{"CH1","CH2","MOVE","SELECT","TRIGGER","PRM","AUTO","SAVE",
      "MENU","UP","DOWN","LEFT","RIGHT","OK","POWER"};
    const unsigned i = static_cast<unsigned>(key); return i < 15 ? names[i] : "?";
  }
  static const char* actionName(link_protocol::KeyAction action) {
    static const char* names[]{"DOWN","UP","REPEAT","LONG"};
    const unsigned i = static_cast<unsigned>(action); return i < 4 ? names[i] : "?";
  }
  const AppManifest manifest_{3, "ESP hardware demo", "0.1.0", "Supply ADC, keys and generated sine"};
  uint32_t lastUpdateMs_ = 0;
  float phase_ = 0;
};
}
App& hardwareDemoApp() { static HardwareDemoApp app; return app; }
}
