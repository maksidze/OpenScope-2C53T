/* USART2 meter bridge (or legacy UART8) + OSLP/1 runtime. */
#ifndef ESP_LINK_H
#define ESP_LINK_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifndef ESP_LINK_BAUD
#ifdef ESP_LINK_BRIDGE
#define ESP_LINK_BAUD 115200u
#else
#define ESP_LINK_BAUD 921600u
#endif
#endif

/* USART2 ISR delegates to this in bridge builds. Meter bytes are encapsulated
 * on the fast segment, unchanged on the ESP-to-meter 9600-baud segment. */
void esp_link_uart_irq(void);
bool esp_link_meter_send(const uint8_t *frame, size_t length);

typedef struct {
    uint32_t rx_bytes;
    uint32_t tx_bytes;
    uint32_t rx_overflow;
    uint32_t tx_overflow;
    uint32_t framing_errors;
    uint32_t crc_or_frame_errors;
    uint32_t valid_frames;
    uint32_t reconnects;
    uint32_t irq_count;
    uint32_t rx_irq_count;
    uint32_t tx_irq_count;
    uint32_t error_irq_count;
    uint32_t last_status;
    uint32_t last_control1;
    uint16_t baud_divider;
    uint8_t uart8_gmux;
    uint8_t pe01_config;
    uint8_t pe0_level;
    uint32_t heap_failures;
    uint32_t task_create_failures;
    uint32_t meter_tx_frames;
    uint32_t meter_rx_bytes;
    uint32_t meter_tx_timeouts;
} esp_link_stats_t;

/* Initializes the selected UART before the scheduler. Bridge builds use
 * PA2/PA3, leave the LCD/XMC bus alone, and keep forwarding session-independent. */
bool esp_link_init(void);
bool esp_link_create_task(void);
/* Dynamic heap required by state, mutex, task stack and TCB, including heap_4
 * block headers/alignment. Used before any link allocation is attempted. */
size_t esp_link_heap_required(void);
void esp_link_note_preflight_failure(void);
/* True once UART8 state/task allocation succeeded, even before HELLO.  This
 * lets the UI distinguish "waiting for bytes" from a disabled link. */
bool esp_link_started(void);
bool esp_link_available(void);
const esp_link_stats_t *esp_link_stats(void);

/* PE0/PE1 share their pad multiplexer with XMC byte-lane signals.  The LCD
 * driver uses this control exchange before temporarily taking the pads for
 * XMC.  QUIESCE waits for the ESP's final ACK; RESUME reopens ESP transmit.
 * These are internal transport hooks, not UI-facing operations. */
bool esp_link_bus_quiesce(uint32_t timeout_ms);
bool esp_link_bus_resume(void);

/* Native ESP Apps launcher hooks. */
void esp_link_request_app_list(void);
void esp_link_request_app_start(uint32_t app_id);
void esp_link_request_app_stop(uint8_t reason);
void esp_link_send_key(uint8_t key_code, uint8_t action, uint16_t held_ms);
void esp_link_ui_ack(uint16_t transaction);

#endif
