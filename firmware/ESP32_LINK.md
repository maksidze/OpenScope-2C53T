# ESP32-Link firmware build

**Current cut-track installation:** use [ESP32_METER_BRIDGE.md](ESP32_METER_BRIDGE.md).
Its USART2 bridge uses `FEATURES="FFT ESP_LINK_LVGL ESP_LINK_BRIDGE"` and never
gates LCD/XMC clocks. The UART8 notes below describe the earlier installation.

The base firmware always includes UART8/OSLP and Device telemetry. Activities
use LVGL 9.5.0 and are opt-in until the dependency footprint is accepted on
hardware:

```sh
cd firmware
git clone --branch v9.5.0 --depth 1 https://github.com/lvgl/lvgl.git lvgl
git -C lvgl rev-parse HEAD   # must be 85aa60d18b3d5e5588d7b247abf90198f07c8a63
make FEATURES="FFT ESP_LINK_LVGL"
```

`firmware/lvgl` is ignored like the other cloned dependencies. The LVGL build
uses a 12-row (7,680-byte) partial RGB565 draw buffer and a 111,104-byte TLSF
heap, both inside the existing 118,784-byte shared lifecycle pool. It adds no
large static RAM object. Entering an Activity requires
`SHMEM_OWNER_ESP_ACTIVITY`; an unexpected owner transition terminates the
Activity instead of allowing stale LVGL pointers.

LVGL tag v9.5.0 is pinned. Its changelog says **XML Engine Removed** and that
development continues outside the main repository; commit `85aa60d` therefore
does not expose a public `lv_xml` runtime loader under `src/`, despite retaining
the XML format/editor documentation. No separately versioned loader dependency
was available to pin and audit with this firmware. Consequently the phase-1
host builds a validated, bounded retained
LVGL model (labels, numeric subjects, chart series and keypad group) from the
versioned UI records in `esp_activity.c`. This is deliberately an LVGL port,
not a private drawing toolkit. The future XML adapter belongs at the same
validated Activity-message boundary when a pinned public runtime API exists.

Dynamic FreeRTOS heap is preflighted only after every native queue, task, and
software timer has been created. Measured from the AT32 debug types, heap_4
cost is 6,888 bytes for the link (4,632-byte state, 2,048-byte task stack,
92-byte TCB, 80-byte mutex, including each block's header/alignment). The
Activity model costs 1,368 bytes without LVGL or 1,488 bytes with LVGL, for
8,256/8,376 bytes total. RX still accepts a 1,024-byte decoded frame by doing
COBS decode in-place; TX uses a 1,024-byte ring and staging bounded to the
actual 192-byte host payload maximum. The three-entry, 384-byte UI queue still
fits a 160-point sine update (330-byte payload).

Before allocating any optional byte, `main()` also reserves 1,760 bytes for
the scheduler's future Idle and Timer-service stacks/TCBs plus 1,024 bytes of
runtime headroom. If that threshold is not available, no ESP `pvPortMalloc`
or UART8 setup occurs. The failure is counted in `esp_link_stats_t` and every
native instrument mode continues normally.

## Browser telemetry (matching ESP32 sketch required)

The current `ESP_LINK_BRIDGE` build adds mode control through the input-task
mailbox and opt-in detailed meter/scope resources. The dashboard is served
by the ESP32 at its AP/Station IP; see `esp32_coprocessor/README.md` and
`PROTOCOL_DRAFT_V0.md` for HTTP usage, wire layouts and preview limitations.
Flash both MCUs; the old minimal meter snapshot remains compatible.

The bridge link state is now 3,456 bytes (32-bit ARM GCC debug types), including
its 256-byte immutable decimated scope cache and metadata. Generated
`esp_link_heap_required()` is 5,712 bytes including task/TCB/mutex/heap headers;
LVGL Activity allocation is another 1,496 bytes. The pre-existing kernel-task
and 1 KiB runtime-headroom preflight remains in force. No framebuffer, client
list, additional task stack or per-viewer AT32 allocation was added. The
192-byte outbound bound, meter ring reservation and 115200 baud are unchanged.
Live heap/stack margins and sustained forwarding still need device testing.
