# ESP32-C3 in the cut meter USART2 tracks

The installation now uses the existing PA2/PA3 UART, not PE0/PE1. Both cut
tracks must stay disconnected between MCU and meter. Four signal wires and
common ground connect the ESP32-C3 Super Mini:

| Signal on the board | ESP32-C3 | Direction | Speed |
|---|---|---|---|
| MCU-side PA2 / USART2 TX | GPIO20 / UART1 RX | MCU -> ESP | 115200 8N1 |
| MCU-side PA3 / USART2 RX | GPIO21 / UART1 TX | ESP -> MCU | 115200 8N1 |
| Meter-side TX, formerly connected to PA3 | GPIO4 / UART0 RX | Meter -> ESP | 9600 8N1 |
| Meter-side RX, formerly connected to PA2 | GPIO5 / UART0 TX | ESP -> meter | 9600 8N1 |
| Ground | GND | Common reference | |

These ESP pin choices are software defaults, not a claim about existing wiring.
GPIO4/5 can be changed in `esp32_coprocessor/src/config.hpp`. Use 3.3-V UART
logic. USB CDC uses GPIO18/19 and remains the console. GPIO21 can emit ROM
boot text before the sketch routes UART0 to GPIO4/5; OSLP CRC/delimiter recovery
rejects it. ESP power and sketch readiness are required for meter operation;
there is no electrical bypass when ESP is reset, unpowered or being flashed.

## Wire format and scheduling

MCU <-> ESP is a full-duplex OSLP/1 stream: COBS, trailing zero, 16-byte header,
CRC32C. Service 0x06 carries opaque native bytes. Original commands remain
10 bytes (the upstream corrected AA55 builder), responses remain unchanged
5AA5/12-byte data and AA55/10-byte echo frames. Unknown meter-side bytes are
also forwarded; only the existing AT32 parser decides what they mean.

The fast segment necessarily adds an envelope. The slow segment receives the
original bytes exactly; raw native bytes and control frames are not mixed by
guessing whether an idle gap is long enough. Bytes inside a native packet can
equal any reserved-looking marker without being mistaken for an ESP command.

MCU sends one meter request and waits for TX_DONE (up to 150 ms). ESP produces
it only after `HardwareSerial::flush()` confirms downstream TX completion.
Timeouts are counted and never replayed automatically: losing an ACK does not
prove the command was not delivered. Before the RTOS, boot uses the identical
envelope and a 200-ms polling deadline. Forwarding is independent of OSLP app
sessions. App restarts, Wi-Fi reconnects and blocking HTTPS run outside the
bridge task and do not stop that task.

Meter RX and TX_DONE have priority between complete envelopes. The currently
active control frame is allowed to finish (<90 ms at the maximum 1024-byte
decoded size and 115200 baud). Control queues are bounded; drops are counted.
This is a software deadline, not a hardware realtime qualification. The
optional AT32 measurement service remains separately enabled on demand.

## Build and flash

ESP: open `esp32_coprocessor/esp32_coprocessor.ino` in Arduino IDE, choose
ESP32C3 Dev Module, core 3.3.8, USB CDC On Boot **Enabled**, then upload.
Both hardware UARTs belong to the bridge; a UART-based Serial console is
incompatible and rejected at compile time.

AT32, from PowerShell in `firmware`:

```powershell
.\build_lvgl.bat rebuild
```

This produces the stock-IAP guest `build/firmware.bin` with
`FEATURES="FFT ESP_LINK_LVGL ESP_LINK_BRIDGE"`. It does not flash automatically.
Without LVGL, use GNU Make (not Embarcadero Make):

```sh
make guest FEATURES="FFT ESP_LINK_BRIDGE"
```

The default feature set includes ESP_LINK_BRIDGE. For the original uncut
9600-baud installation omit that feature explicitly, for example FEATURES=FFT.
Do not use the uncut installation's image on the cut-track board. Change the
fast baud only in **both** `ESP_LINK_BAUD` and `config::kScopeUartBaud`; keep
the downstream at 9600. Qualify 115200 first before trying 230400 or 460800.

## Acceptance on real hardware

1. With ESP USB powered, its console should show `meter bridge ready`, host
   baud 115200 and meter baud 9600. A USB terminal is not required for forwarding.
2. On a logic analyzer, decode each side at its own speed. Native 10-byte TX
   payloads inside MCU OSLP must match the meter-side bytes exactly. Compare
   the reverse stream too, including zeros and split response packets.
3. In meter mode, check that `[bridge] meter tx` and `rx` both grow. `errors`
   should settle after boot; `control_drops` shows app pressure. On AT32,
   `esp_link_stats()` additionally exposes meter TX completions/timeouts and
   forwarded meter RX bytes. Counters alone do not prove pad waveforms.
4. Select several meter functions. Run the HTTPS demo and switch Wi-Fi modes
   while capturing meter data; verify no bridge errors, false readings or
   persistent FIFO backlog. Re-enter the meter after using the scope.
5. Reset each device separately. App HELLO should recover. A command whose
   delivery timed out is not automatically repeated. Powering ESP off removes
   the meter connection until it starts again.

## Display stack and Wi-Fi updates

The LVGL build gives the `display` task 1536 Cortex-M4 words (6144 bytes).
Native-only builds retain 768 words (3072 bytes). With `LV_OS_NONE`, nested
screen/label events and software glyph drawing all run on this same task.
The former 3072-byte budget was too small for the roughly 3000-byte audited
text-render chain plus ARM exception/FPU context; Wi-Fi connection-status text
updates exposed a `STACK OVERFLOW - display` on the unit.

The extra stack comes from the existing 31-KiB FreeRTOS heap, before ESP
preflight. Neither the heap array nor the shared LVGL arena is enlarged.
Settings redraw requests no longer call LVGL under the native settings draw
stack; the top-level display loop services it once per iteration.

After rebuilding, run:

```powershell
python firmware/tests/check_display_stack_budget.py
```

The Windows build wrapper runs this check automatically. The compiler writes
per-function `.su` reports to `firmware/build`. The audit
reserves exception/FPU context and additional margin and includes the old
3072-byte budget as a failing negative control. It checks a reviewed text path,
not every possible recursive LVGL path or actual on-device high-water mark.

The AT32 USB debug command `status` now prints heap current/minimum free,
`Task display stack minimum free: ... B` and the allocated display stack size.
Exercise AP -> STA -> connected, long SSIDs/IP text, chart updates and repeated
app entry/exit, then read these values to qualify the margin on real hardware.
The ESP sketch does not need reflashing for the stack-only correction. The
subsequent full Wi-Fi settings/keyboard change below does require both sides.

## On-device Wi-Fi settings and keyboard

Flash both updated firmwares. **ESP32 Apps -> Wi-Fi settings** now provides
live status/IP/RSSI, asynchronous network discovery, hidden-network SSID entry,
saved-network reconnect/forget, and AP name/password/channel editing. A router
connection is not labelled as a verified Internet connection. Details and all
controls are in `esp32_coprocessor/README.md`, Wi-Fi operation.

The keyboard is one generic LVGL buttonmatrix, not 50 independently allocated
widgets. ESP handles its navigation; AT32 retains only its bounded captions
and selected index. UP/DOWN/LEFT/RIGHT moves, OK types, PRM deletes, SAVE
finishes and MENU cancels. Passwords are hidden by default. Only explicit
confirmation saves a versioned NVS profile in ESP; cancelled drafts do not
change settings. The old browser setup remains as an optional fallback.

UI records now use stop-and-wait transactions: one record in flight, ACK after
display-task application, retry after 800 ms, duplicate suppression on AT32.
This prevents initial definitions from overflowing the host's two-slot queue.
Unsent value/selection updates coalesce; screen changes discard stale pending
records without mutating the in-flight retry copy. No AT32 task or RTOS heap
array was added. The stack audit covers the new keyboard text path as well
as ordinary labels (3504/3000 bytes before the 720-byte context/margin reserve).

Host tests run production `meter_bridge.cpp`/`link.cpp` with fake UARTs/RTOS
and the actual independent AT32 C codec. They exercise byte preservation,
zero-containing commands, routing, CRC rejection, truncation/oversize recovery,
completion-after-flush order and bounded control queue saturation. These tests
do not replace the physical timing and electrical acceptance above.

On Windows with Visual Studio C++ Build Tools:

```powershell
.\esp32_coprocessor\tests\run_bridge_tests.bat
```

With GCC/Clang, run `python esp32_coprocessor/tests/run_bridge_tests.py`.
The runner also checks the upstream meter command builder and function-word
mapping. A Zig installation can be selected with `--zig /path/to/zig`.

Verified on 2026-10-18 after Wi-Fi UI changes: the Windows build produces a
899,660-byte AT32 guest image with FFT/LVGL/bridge. Arduino-ESP32 3.3.8 builds
the C3 sketch (1,182,315 bytes reported, 55,412 bytes static RAM). Tests pass: production
bridge 2,352 checks; UI ACK/retry/coalescing 36; actual Wi-Fi app driven through
button events and independently decoded UI frames 380; keyboard/validation
623; production Wi-Fi manager against fake radio/NVS/HTTP edges 45; C OSLP
15; both upstream meter command suites. They cover masked entry, explicit
save, cancelled AP/manual edits, scan deduplication/order/bounds, no-DHCP,
auth/no-SSID errors, fallback recovery, persistence and save failure.
The prior legacy FEATURES=FFT guest also built. Hardware screen appearance,
radio association, bridge timing, power integrity and readings still require
qualification on the cut-track unit; host checks do not prove those.

## Upstream synchronization

On 2026-10-18, origin/main was fast-forwarded from d4b7def to upstream/main
ab57f04 (212 commits). Local esp32 merged upstream as eb49a3d. Existing local
work was restored and conflicts resolved retaining both upstream and ESP UI.
The pre-sync state is recoverable from `backup/esp32-before-upstream-20261018`
and the stash named `ESP32 work before upstream sync 2026-10-18`.
