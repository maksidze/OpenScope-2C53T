# OpenScope ESP32-C3 co-processor — Arduino sketch

Arduino IDE firmware for an ESP32-C3 Super Mini connected to the FNIRSI 2C53T
OpenScope firmware. ESP32 owns Wi-Fi, HTTP and application behaviour. AT32 owns
the 320x240 display, LVGL widgets, physical buttons and system POWER policy.

The current hardware installation uses ESP32 **in the cut USART2 meter lines**:
MCU-to-ESP is 115200 8N1; ESP-to-meter stays 9600 8N1. It requires matching
AT32 firmware built with `ESP_LINK_BRIDGE`. See
[the bridge wiring/build/bench guide](../firmware/ESP32_METER_BRIDGE.md).

## What is included

- OSLP/1 UART transport: 16-byte header, COBS, CRC32C, sessions, transactions,
  HELLO/heartbeat and parser recovery;
- independent high-priority meter bridge task, active before Wi-Fi or USB wait;
- generic retained LVGL view descriptors and subject updates;
- application registry with three applications:
  - `Wi-Fi settings` — status and AP/STA switching;
  - `Unix time (HTTP)` — GET `https://timeapi.io/api/v1/time/current/unix`, OK repeats;
  - `ESP hardware demo` — key events, generated sine chart and supply ADC;
- independent, opt-in AT32 measurement service, toggled by PRM in the hardware demo;
- true AP-only and STA-only configured modes;
- temporary fallback AP after five unsuccessful STA reconnects;
- Preferences/NVS persistence for STA SSID/password, AP SSID/password/channel and mode;
- browser instrument dashboard at `/`, in both AP and Station modes;
- Wi-Fi setup page at `/wifi-settings` (also linked from the dashboard).

## Live measurements in a PC browser

Flash **both** the matching AT32 build (`firmware/build_lvgl.bat rebuild`) and
this Arduino sketch. No extra libraries, PC application, CDN or internet
connection are needed. In Station mode open `http://<ESP32 IP>/` from a PC on
the same LAN (for example `http://192.168.1.200/` if that is the current lease).
In AP mode connect the PC to the saved OpenScope AP and open
`http://192.168.4.1/`. Router client isolation can prevent LAN access.

The Russian-language dashboard provides:

- Oscilloscope / Multimeter / Generator / Settings buttons. Commands are
  queued to the AT32 input task and confirmed by actual mode telemetry, not
  by an optimistic HTTP success. Generator entry asks for confirmation;
- meter display text, actual range unit (including mV/mA/kOhm etc.), HOLD,
  overload, rejection and stale-data states;
- two-channel Canvas waveform rendering **from raw ADC samples**, never LCD
  pixels, screenshots or LVGL drawing commands;
- calibrated sample-rate and volts-per-count metadata where available,
  retaining provisional tiers and unknown scales rather than invented units;
- automatic reconnection and hiding of stale/unavailable measurements.

The first waveform transport is deliberately a **preview**: 128 samples per
channel, stride 8 across the AT32's 1024-point buffers, up to 5 captures/s;
the page polls sequentially every 250 ms. Decimation can alias fast signals
and miss narrow pulses. It is not an anti-aliased/full-record export or a
precision measurement engine. The plot vertically auto-fits ADC counts;
it does not claim a calibrated DC baseline or V/div graticule. Unknown time
calibration uses original sample indices. Stopped captures remain viewable;
unchanged running captures are marked stale after two seconds. Roll-specific
buffers are not exported in this initial record-preview protocol.

Opening/polling `/api/live` grants a five-second web lease to the independent
MeasurementService. Hidden tabs stop polling. After the last viewer stops,
the ESP unsubscribes; an independent ten-second AT32 subscription lease also
bounds abandoned traffic. The hardware-demo PRM switch is a **separate**
manual owner, so closing the browser does not disable a deliberately enabled
background service. Subscription renewal/recovery survives link reconnection.
No additional FreeRTOS tasks or AT32 per-viewer allocations are introduced.
Meter forwarding always retains its own priority and UART queue reservation.

Local API (also usable by a future native PC client):

```text
GET  /api/live           JSON API version 1; active mode, meter, scope arrays,
                        sample stride/rate/calibration tiers, age and errors
POST /api/mode           form mode=0|1|2|3; X-OpenScope-Control: 1 required
GET  /wifi-settings      existing Wi-Fi configuration portal
```

`/api/mode` returns 202 for queue acceptance, 400 for invalid mode, 403 for a
missing control header/cross-origin browser request, 409 for another pending
transition, or 503 for offline AT32. Poll `control.pending`, `control.error`
and `mode` for the outcome. A matching UART ACK alone is not mode confirmation.
Mutations are POST-only, with same-origin checks and no CORS grants. This is
CSRF mitigation, **not authentication or TLS**: use only a trusted LAN/AP,
do not port-forward this service or expose it to the internet. Measurement
snapshots never include Wi-Fi credentials. HTTP remains plaintext.

Verification (no connected hardware is opened):

```powershell
.\esp32_coprocessor\tests\run_bridge_tests.bat
node esp32_coprocessor/tests/test_dashboard.mjs
# Optional Edge headless screenshots with clearly labelled SYNTHETIC data:
node esp32_coprocessor/tests/preview_dashboard.mjs
node esp32_coprocessor/tests/preview_dashboard.mjs --meter
```

The native suite executes the production telemetry service and HTTP routes,
including truncation, missing/mixed waveform chunks, JSON escaping/capacity,
leases, reboot revisions, late command errors and mode acceptance/confirmation.
Build/tests and synthetic browser previews do not prove live FPGA data or
on-device heap/stack high-water marks; verify those after flashing both MCUs.

The fixed draft wire profile is documented in
[PROTOCOL_DRAFT_V0.md](PROTOCOL_DRAFT_V0.md). It is byte-compatible with the
current AT32 host implementation. It deliberately remains draft v0 until the
proposed CBOR tables and complete device catalog are frozen.

## Arduino IDE setup

Tested with:

- Arduino IDE 2.x;
- `esp32 by Espressif Systems` core **3.3.8**;
- board `ESP32C3 Dev Module` (use a dedicated `ESP32-C3 Super Mini` definition
  if your board package provides one);
- 4 MB flash;
- USB CDC On Boot: Enabled, for `Serial` diagnostics.

No third-party libraries are required. These libraries come with the
Arduino-ESP32 core: WiFi, Preferences, WebServer, HTTPClient,
WiFiClientSecure and HardwareSerial.

Open [esp32_coprocessor.ino](esp32_coprocessor.ino) in Arduino IDE, select the
board and USB port, then click Verify or Upload. Arduino IDE recursively
compiles the implementation in `src/`.

Equivalent CLI check:

```powershell
arduino-cli compile --fqbn "esp32:esp32:esp32c3:CDCOnBoot=cdc" esp32_coprocessor
```

Verified with core 3.3.8: compilation succeeded with USB CDC enabled.
The bridge sketch uses 1,164,509 bytes (88%) of the default 1,310,720-byte app
partition and 45,252 bytes (13%) of static RAM (task, queue and UART buffers
are additional runtime allocations). Warnings emitted by
`--warnings all` are confined to the core's own ESP32-C3 SPI flash HAL headers;
the sketch sources compile without diagnostics.

## Wiring

```text
AT32 PA2 / USART2_TX (MCU side)  --> ESP32-C3 GPIO20 RX / UART1
AT32 PA3 / USART2_RX (MCU side)  <-- ESP32-C3 GPIO21 TX / UART1
Meter TX (other side of PA3 cut) --> ESP32-C3 GPIO4 RX / UART0
Meter RX (other side of PA2 cut) <-- ESP32-C3 GPIO5 TX / UART0
AT32 GND ------------------------- ESP32-C3 GND
```

Both endpoints use 3.3 V logic. This installation routes the scope link through
GPIO20/21; native USB CDC remains on GPIO18/19. GPIO20/21 are also the C3 ROM
UART0 defaults, so boot-ROM text may briefly appear on GPIO21 before the sketch
routes HardwareSerial 1 there. Change the application mapping in
`src/config.hpp`. Initial tests should power ESP separately. An internal install
needs a Wi-Fi-peak-capable regulator, local decoupling and analog noise-floor
testing with RF off, idle and transmitting.

## Wi-Fi operation

Open **ESP32 Apps -> Wi-Fi settings** on the oscilloscope. No phone or browser
is required. Flash both the updated AT32 and ESP firmwares: the generic LVGL
button-matrix records and reliable UI acknowledgements are new on both sides.

- Main screen: live connection state, selected SSID, IP address, RSSI when
  connected, retry count/errors, fallback AP and AP client count. "Connected
  to router" means association plus a DHCP address, not a verified Internet
  connection. Use the Time API app to test the external HTTP path.
- **Scan Wi-Fi networks** lists up to 20 strongest unique SSIDs, with RSSI and
  open/locked indicators; UP/DOWN selects, LEFT/RIGHT pages, OK opens password
  entry. AUTO rescans; scanning is asynchronous and does not stop the bridge.
- **Enter SSID manually** supports hidden networks. Enter an empty password
  only for an open network; the confirmation explicitly warns about encryption.
- Keyboard: arrows move the highlight; OK types/activates a key. `abc`, `ABC`,
  `#+=` switch pages; `SP` inserts a space; `Del`/PRM deletes; `Clr` clears;
  `Hid`/`Vis` hides/reveals a password; keyboard `OK` or physical SAVE finishes;
  `Bk` or MENU cancels. All 95 printable ASCII characters are accessible.
  SSIDs accept 1-32 bytes; WPA passwords accept 8-63 printable ASCII characters
  (station also accepts 64 hexadecimal PSK digits). Scanned UTF-8 SSIDs are
  retained byte-for-byte, but manual keyboard input is ASCII-only.
- **Save and connect** explicitly commits router credentials, then shows
  progress on the main screen. **Connect saved Wi-Fi** retries the saved
  profile without re-entering its password. **Forget saved Wi-Fi** requires
  confirmation and returns to AP mode.
- **Configure access point** edits AP SSID, masked password and channel.
  LEFT/RIGHT changes the selected channel row. Nothing is stored until
  **Save and enable AP**. MENU cancels. **Use access point mode** activates
  the previously saved AP without discarding the station profile.

First boot starts `OpenScope-XXXX`, password `openscope`. The optional browser
setup at `192.168.4.1` remains available. AP and STA are distinct saved modes.

If STA connection fails five times, ESP temporarily enables the saved AP
without changing the saved mode. It keeps retrying STA and removes fallback AP
after connecting. Each association/DHCP attempt has 15 seconds, rather than
aborting potentially healthy connections after two seconds. Only 2.4 GHz
personal/open Wi-Fi is supported by this setup, not enterprise/802.1X login.

Credentials are versioned Preferences/NVS blobs (one atomic blob per profile),
with backward reading of older separate keys. Save failures are reported, not
presented as success. Passwords are masked by default and are not logged.
Credentials are not encrypted; the browser portal is plain HTTP and a password
deliberately revealed on screen travels over the local UART. Production hardening should
add NVS/flash encryption and a physical credential-reset gesture.

## ESP supply measurement

ESP32-C3 has no documented internal ADC channel that measures its own VDD. The
demo requires an external divider to GPIO0, for example:

```text
3V3 ---- 100k ----+---- GPIO0
                  |
                 100k
                  |
GND --------------+
```

The ratio is configured in `src/config.hpp`. The requested guarded call is
preserved exactly:

```cpp
SET_PERI_REG_MASK(APB_SARADC_APB_ADC_CLKM_CONF_REG, APB_SARADC_CLK_EN);
```

It only enables the SARADC clock. Measurement uses 16 averaged
`analogReadMilliVolts()` readings plus the divider ratio. Floating or saturated
input displays N/A. Keep the ADC input within the ESP32-C3 datasheet limits.

## TLS note

The Time API demo uses `WiFiClientSecure` in explicitly marked development
`setInsecure()` mode because Arduino-ESP32 does not automatically attach a
system CA bundle. It proves the HTTP application path but does not authenticate
the server. Before trusting network results, provide an appropriate root CA
with `setCACert()` and define certificate rotation policy.

## Structure

```text
esp32_coprocessor.ino       Arduino setup()/loop()
src/config.hpp              pins, baud, ADC ratio and timeouts
src/link.*                  OSLP framing and UART session
src/meter_bridge.*          UART ownership, opaque forwarding, priority arbitration
src/app_runtime.*           manifests, lifecycle and UI subjects
src/wifi_manager.*          WiFi/Preferences/WebServer state machine
src/measurement_service.*   opt-in AT32 telemetry cache
src/instrument_web.*        dashboard and local JSON/control HTTP routes
src/dashboard_page.hpp      self-contained HTML/Canvas client in ESP flash
src/telemetry_json.hpp      bounded JSON serializer
src/apps/                   three applications
tests/protocol_selftest.py  dependency-free COBS/CRC golden check
tests/test_meter_bridge.cpp production bridge test against AT32 C codec/fake UARTs
```

Add an app by implementing `App`, describing its screen through `UiClient`,
and registering its singleton in the `.ino`. Applications never receive AT32
pointers, registers or executable code.
