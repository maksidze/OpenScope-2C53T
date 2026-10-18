/*
 * OpenScope Link Protocol v1 (OSLP/1) transport definitions.
 *
 * This is the normative AT32 wire header shared conceptually with the ESP32
 * coprocessor.  It intentionally does not include FreeRTOS or MCU headers so
 * COBS/CRC/frame validation can be host-tested without hardware.
 */
#ifndef ESP_LINK_PROTOCOL_H
#define ESP_LINK_PROTOCOL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define OSLP_MAGIC_0                 0x4Fu /* 'O' */
#define OSLP_MAGIC_1                 0x53u /* 'S' */
#define OSLP_VERSION_MAJOR           1u
#define OSLP_VERSION_MINOR           0u
#define OSLP_HEADER_SIZE             16u
#define OSLP_CRC_SIZE                4u
#define OSLP_MAX_DECODED_FRAME       1024u
#define OSLP_MAX_PAYLOAD             (OSLP_MAX_DECODED_FRAME - OSLP_HEADER_SIZE - OSLP_CRC_SIZE)
#define OSLP_MAX_ENCODED_FRAME       (OSLP_MAX_DECODED_FRAME + (OSLP_MAX_DECODED_FRAME / 254u) + 2u)

typedef enum {
    OSLP_KIND_REQUEST  = 1,
    OSLP_KIND_RESPONSE = 2,
    OSLP_KIND_EVENT    = 3,
    OSLP_KIND_STREAM   = 4,
    OSLP_KIND_ACK      = 5,
    OSLP_KIND_ERROR    = 6,
} oslp_kind_t;

#define OSLP_FLAG_CBOR              (1u << 0)
#define OSLP_FLAG_RAW               (1u << 1)
#define OSLP_FLAG_MORE              (1u << 2)
#define OSLP_FLAG_REPLAY            (1u << 3)
#define OSLP_FLAG_URGENT            (1u << 4)

typedef enum {
    OSLP_SERVICE_CORE   = 0x00,
    OSLP_SERVICE_APP    = 0x01,
    OSLP_SERVICE_UI     = 0x02,
    OSLP_SERVICE_ASSET  = 0x03,
    OSLP_SERVICE_STREAM = 0x04,
    OSLP_SERVICE_DEVICE = 0x05,
    OSLP_SERVICE_METER_BRIDGE = 0x06,
} oslp_service_t;

/* Stable phase-1 opcode allocation. Payloads are little-endian raw binary
 * until the bounded CBOR codec and LVGL XML loader are enabled. Every string
 * is u8 byte_length followed by UTF-8 bytes without a trailing NUL.
 *
 * CORE/HELLO request and response:
 *   u8 abi, u8 protocol_major, u8 protocol_minor, u16 max_decoded_frame,
 *   u32 boot_nonce, u8 service_count, u8 heartbeat_seconds,
 *   u8 max_outstanding. The request may use envelope session=0; the AT32
 *   response assigns the non-zero session used by all later operations.
 * CORE/HEARTBEAT has no payload.
 *
 * APP/LIST request: u8 abi.
 * APP/LIST response:
 *   u8 abi, u8 count, repeated { u16 app_id, string name, string version,
 *   string description }.
 * APP/START request: u8 abi, u16 app_id.
 * APP/START response: u8 abi, u16 app_id, u32 activity_token.
 * APP/STOP request: u8 abi.
 * APP/INPUT event:
 *   u8 abi, u32 activity_token, u8 zero_based_key, u8 action
 *   (0=press, 1=release). POWER is never sent to the ESP.
 *
 * UI/ACTIVITY_STATE request begins with u8 abi, u8 record, u32 token.
 * Stop-and-wait: host ACKs the transaction only after applying a record in
 * the display task. Retry with identical transaction/payload after 800 ms;
 * host re-ACKs the last applied transaction without applying it twice.
 *   0x01 SCREEN_BEGIN: u16 app_id, string title
 *   0x02 LABEL_DEFINE: u16 subject, i16 x,y,w,h, u8 font, string initial
 *   0x03 CHART_DEFINE: u16 subject, i16 x,y,w,h, u16 capacity
 *   0x05 GRID_DEFINE: u16 subject, i16 x,y,w,h, u8 columns, u8 count,
 *                     count * string caption (1..50 buttons, 1..10 columns)
 *   0x04 READY: no fields
 *   0x10 TEXT_SET: u16 subject, string value
 *   0x11 FLOAT_SET: u16 subject, IEEE754 f32 value, string unit
 *   0x12 SERIES_SET: u16 subject, u16 count, count*i16 samples.
 *   0x13 GRID_SELECT: u16 subject, u16 zero-based button index.
 * Grid maps are retained in the bounded LVGL heap, freed on screen teardown;
 * navigation/actions are handled by the ESP app, not a second host key map.
 * The host bounds subjects, strings, geometry, chart capacity, and queueing.
 *
 * DEVICE/SUBSCRIBE request:
 *   u8 abi, u16 resource_id, u32 period_us, u8 delivery_policy.
 * DEVICE/UNSUBSCRIBE and DEVICE/SNAPSHOT request: u8 abi, u16 resource_id.
 * Meter snapshot/event (resource 0x1001):
 *   u8 abi, u16 resource_id, u32 revision, IEEE754 f32 value,
 *   u8 unit, u8 quality. Other AT32 resources use the richer catalogued
 *   prefix documented at their encoder because they are not consumed by the
 *   current ESP MeasurementService proof application.
 */
#define OSLP_CORE_HELLO             0x01u
/* Session-independent transparent UART bridge. Payload contains ONLY original
 * instrument bytes. TX_DONE acknowledges physical downstream stop-bit delivery;
 * it is not a meter ACK. Never automatically replay a timed-out command. */
#define OSLP_METER_TX               0x01u
#define OSLP_METER_RX               0x02u
#define OSLP_METER_TX_DONE          0x03u
#define OSLP_CORE_HEARTBEAT         0x02u
#define OSLP_CORE_BUS_QUIESCE       0x03u
#define OSLP_CORE_BUS_RESUME        0x04u
#define OSLP_APP_LIST               0x01u
#define OSLP_APP_START              0x02u
#define OSLP_APP_STOP               0x03u
#define OSLP_APP_INPUT              0x04u
#define OSLP_UI_ACTIVITY_STATE      0x01u
#define OSLP_DEVICE_CATALOG         0x01u
#define OSLP_DEVICE_SUBSCRIBE       0x02u
#define OSLP_DEVICE_UNSUBSCRIBE     0x03u
#define OSLP_DEVICE_SNAPSHOT        0x04u
#define OSLP_DEVICE_SET_MODE        0x05u

/* Stable phase-1 resource IDs. */
#define OSLP_RESOURCE_UPTIME        0x0001u
#define OSLP_RESOURCE_BATTERY       0x0002u
#define OSLP_RESOURCE_DEVICE_MODE   0x0003u
#define OSLP_RESOURCE_METER_READING 0x1001u
#define OSLP_RESOURCE_METER_DETAIL  0x1002u
#define OSLP_RESOURCE_SCOPE_PREVIEW 0x2001u

/* Raw payload ABI version. Every raw operation starts with this byte. */
#define OSLP_RAW_ABI_VERSION        0u

typedef struct {
    uint8_t kind;
    uint8_t flags;
    uint8_t service;
    uint8_t opcode;
    uint16_t session_id;
    uint16_t transaction_id;
    uint16_t sequence;
    uint16_t payload_length;
    const uint8_t *payload;
} oslp_frame_t;

uint32_t oslp_crc32c(const uint8_t *data, size_t length);
size_t oslp_cobs_encode(const uint8_t *input, size_t length,
                        uint8_t *output, size_t capacity);
size_t oslp_cobs_decode(const uint8_t *input, size_t length,
                        uint8_t *output, size_t capacity);

/* Build a decoded header|payload|CRC frame. Returns 0 on invalid input or
 * insufficient capacity. The caller COBS-encodes this result for UART. */
size_t oslp_frame_build(const oslp_frame_t *frame,
                        uint8_t *output, size_t capacity);

/* Validate magic/version/length/CRC and expose a payload view into decoded.
 * The decoded buffer must remain alive while the returned view is used. */
bool oslp_frame_parse(const uint8_t *decoded, size_t length,
                      oslp_frame_t *frame);

#endif
