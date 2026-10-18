# OpenScope ESP link — draft payload profile v0

This firmware implements the OSLP/1 envelope described by
`docs/design/esp32_link_activity_runtime.md` in the OpenScope repository:

- MCU-to-ESP UART 8N1, 115200 baud; ESP-to-meter UART 9600 baud;
- 16-byte little-endian `OS` header;
- CRC32C (Castagnoli) over header and payload;
- COBS encoding followed by `00`;
- 1024-byte maximum decoded frame.

The architecture document intentionally leaves numeric opcodes, CBOR keys and
the LVGL bundle ABI open. This directory therefore uses a **non-stable draft
v0 binary payload** so both processors can be brought up now. Every application payload
begins with byte `payload_version = 0`. Replace these records with normative
CBOR after its integer-key table is frozen; do not silently reinterpret v0.

## Assigned services and opcodes

| Service | ID | Opcodes |
|---|---:|---|
| Core | `00` | `01 HELLO`, `02 HEARTBEAT`, `03 BUS_QUIESCE`, `04 BUS_RESUME` |
| Application | `01` | `01 LIST`, `02 START`, `03 STOP`, `04 INPUT` |
| UI | `02` | `01 ACTIVITY_MESSAGE` |
| Asset | `03` | reserved |
| Stream | `04` | reserved |
| Device | `05` | `01 CATALOG`, `02 SUBSCRIBE`, `03 UNSUBSCRIBE`, `04 SNAPSHOT` |
| Meter bridge | `06` | `01 TX`, `02 RX`, `03 TX_DONE` |

## Transparent meter bridge (current hardware)

The MCU-side wire is entirely OSLP. The 10 native command bytes are carried
unchanged in `service=06, opcode=01, kind=Request`. The bridge neither patches
the AA55 header nor recomputes the original checksum. `transaction_id` identifies
the command. `TX_DONE` is an ACK with the same transaction ID after the final
9600-baud stop bit; errors use `kind=Error`. The MCU waits up to 150 ms and never
automatically retries an uncertain delivery. Early boot uses the same protocol
with a 200-ms polling deadline before FreeRTOS starts.

Every meter-side RX byte is wrapped in ordered `service=06, opcode=02,
kind=Stream` records (1..64 payload bytes). Chunks may split original frames.
No inner header filtering is applied. These three operations are deliberately
session-independent, so forwarding does not depend on HELLO, Wi-Fi or an app.
Their payload has **no ABI-version prefix**: all bytes are original meter data.

The ESP bridge owns both UARTs in its own task. Meter RX and completion records
are sent before the next queued application frame; an active frame is never
interrupted. The MCU TX ring reserves space for a full meter command envelope.
The optional measurement service remains independent of this essential bridge.

## Historical PE0/PE1 physical-bus barrier (not used by bridge builds)

AT32 UART8 and XMC LB/UB share PE0/PE1. A display transaction therefore uses
an explicit barrier rather than relying on an observed idle interval:

- `BUS_QUIESCE request`: `v:u8, max_hold_ms:u16`; its response is `v:u8` and is the last
  ESP-to-AT32 frame before the ESP becomes silent;
- the ESP flushes the response through the UART hardware and rejects ordinary
  UI, application, measurement and heartbeat sends while quiesced;
- after the AT32 has finished its XMC transaction and restored UART8,
  `BUS_RESUME event`: `v:u8` re-enables ESP transmission;
- a one-second ESP fail-safe abandons the session and resumes HELLO discovery
  if RESET or a damaged frame prevents BUS_RESUME from arriving.

The AT32 may take an offline display grant without a response when no session
exists. Repeated HELLO frames provide recovery after such a blackout.

## Application records

All integers are little-endian. Strings are `u8 length` followed by UTF-8.

- `LIST response`: `v:u8, count:u8, repeated{id:u16,name,version,description}`.
- `START request`: `v:u8, app_id:u16`.
- `START response`: `v:u8, app_id:u16, activity_token:u32`.
- `STOP`: `v:u8` (a future revision adds the reason and token).
- `INPUT`: `v:u8, activity_token:u32, key:u8, action:u8`.

Keys are `CH1, CH2, MOVE, SELECT, TRIGGER, PRM, AUTO, SAVE, MENU, UP, DOWN,
LEFT, RIGHT, OK, POWER` numbered from zero. Actions are `DOWN=0, UP=1,
REPEAT=2, LONG=3`. POWER remains host-reserved; an Activity must never delay
or veto the AT32 shutdown/escape path.

## Generic LVGL view records

`UI/ACTIVITY_MESSAGE` begins `v:u8, message:u8, activity_token:u32`.

| Message | ID | Remaining fields |
|---|---:|---|
| `SCREEN_BEGIN` | `01` | `app_id:u16, title:string` |
| `LABEL_DEFINE` | `02` | `subject:u16, x/y/w/h:i16, font:u8, initial:string` |
| `CHART_DEFINE` | `03` | `subject:u16, x/y/w/h:i16, capacity:u16` |
| `SCREEN_READY` | `04` | none |
| `TEXT_SET` | `10` | `subject:u16, value:string` |
| `FLOAT_SET` | `11` | `subject:u16, value:f32, unit:string` |
| `SERIES_SET` | `12` | `subject:u16, count:u16, values[count]:i16` |

These are generic retained view descriptors and subject updates, not remote
drawing calls. The AT32 side creates LVGL widgets, owns focus and rendering,
and validates coordinates, subjects, sizes and activity tokens.

## Measurement service draft

The opt-in background service currently uses provisional resource ID `1001`
for `meter/reading`:

- `SUBSCRIBE`: `v, resource_id:u16, period_us:u32, policy:u8`;
- `UNSUBSCRIBE`: `v, resource_id:u16`;
- `SNAPSHOT`: `v, resource_id:u16, revision:u32, value:f32, unit:u8, quality:u8`.

The legacy `0x1001` snapshot remains wire-compatible. The browser/background
service now subscribes to `0x0003` mode, `0x0002` battery, `0x1002` meter detail
and `0x2001` scope preview. Subscribe renewals grant a ten-second AT32 lease;
unsubscribe or session replacement clears the relevant stream. Minimum
periods are 250 ms for meter detail, 200 ms for scope, 100 ms for mode and
1000 ms for battery. Browser ownership itself expires in five seconds; the
hardware-demo PRM flag is independent. All numeric fields below are LE.

`DEVICE/SET_MODE = 0x05` request is `v:u8, mode:u8` (0 scope, 1 meter, 2 signal
generator, 3 settings). ACK echoes this pair and means **queued**. Only the
input task executes the shared native mode transition; modal UI rejects it as
busy. Device-mode telemetry, not ACK, confirms completion. Error payload is
`v:u8, code:u8` (1 malformed, 2 unsupported, 3 busy). ESP uses a distinct
transaction per user command and retries the same transaction for that command.

Mode snapshot is 14 bytes: `v:u8, resource:u16, revision:u32, timestamp_ms:u32,
quality:u8, type:u8=3, mode:u8`. Battery uses the same prefix with type 2,
then percent:u8, millivolts:u16, charging:u8 (17 bytes total).

Meter detail (`0x1002`) is exactly 44 bytes:

```text
offset 0:  v:u8=0, resource:u16=0x1002, revision:u32, timestamp_ms:u32
offset 11: quality:u8 (0 valid, 2 overload, 5 rejected, 6 unavailable)
offset 12: submode:u8, result_class:u8, reject_reason:u8
offset 15: flags:u8 (bit 0 HOLD, bit 1 auto range, bit 2 AC diagnostic)
offset 16: value:f32, display:char[16], unit:char[8] (NUL padded)
```

Display/unit strings come from the authoritative meter parser. Never infer
unit scaling from submode; rejected/overload values are not valid numbers.

Scope preview (`0x2001`) is subscribe-only. Each coherent preview has two
158-byte `DEVICE/SNAPSHOT` events, each holding 64 raw u8 samples per channel:

```text
offset 0:  v:u8=0, resource:u16=0x2001, acquisition_generation:u32
offset 7:  preview_timestamp_ms:u32
offset 11: flags:u8 (bit 0 valid, bit 1 RUN, bits 2/3 channel enabled,
                     bit 4 acquisition record is time-ordered)
offset 12: part:u8=0|1, timebase_code:u8, sample_stride:u8=8
offset 15: acquisition_sample_rate_hz:f32 (0 means unknown)
offset 19: CH1_volts_per_count:f32, CH2_volts_per_count:f32 (0 unknown)
offset 27: CH1_gain_tier:u8, CH2_gain_tier:u8, timebase_tier:u8
offset 30: CH1_raw:u8[64], CH2_raw:u8[64]
```

Tiers: 0 unknown, 1 provisional, 2 measured (existing scope calibration
contracts). Sample `i` in the assembled 128-point channel is original index
`i*8`. Time differences are `delta_index*stride/acquisition_sample_rate`.
No absolute DC zero is asserted. Do not divide by the decimated rate twice.
Do not derive precision frequency/peak measurements from this lossy preview.

AT32 copies a 256-byte decimated record, checking the acquisition seqlock and
scope configuration before/after capture and metadata. It sends one part per
link-task tick with normal meter-priority backpressure. Both parts must have
the same generation AND timestamp; ESP publishes only a complete pair and
abandons incomplete/mixed pairs. Same generation does not reset freshness.
Generation zero (no staging) is refused rather than exporting a torn record.
No stream contains screen pixels. A new session clears caches/revision domains.
