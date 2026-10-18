/*
 * ESP32-C3 coprocessor link and phase-1 OSLP runtime.
 * ESP_LINK_BRIDGE selects USART2 PA2(TX)/PA3(RX), 115200 8N1, with opaque
 * downstream meter bytes on service 0x06. The ESP's second UART stays 9600.
 * IRQs only move ring bytes; original meter parsing runs in the link task.
 * This route does not touch PE0/PE1, UART8 or XMC clock ownership.
 *
 * The older standalone UART8 build below is retained behind the feature flag.
 *
 * Hardware source of truth:
 *   - the AT32F403A V2.04 datasheet assigns PE0=UART8_RX and PE1=UART8_TX;
 *   - UART8_GMUX would move those signals to PC2/PC3, so it is explicitly
 *     cleared here.  This also makes a stock-IAP/guest handoff independent of
 *     remap state left by code that ran before this image;
 *   - both MCUs use 3.3 V logic, with 470R..1k series resistors recommended
 *     on both UART wires by docs/design/esp32_link_activity_runtime.md.
 *
 * PE0/PE1 routing is hardware-documented but still unverified on a populated
 * 2C53T-V1.4 board.  The initial 921600 baud is therefore a requested target,
 * not an empirical signal-integrity claim; a 115200 build and one-hour soak
 * remain the acceptance gate. UART ISR code only moves ring bytes. COBS, CRC,
 * lifecycle, and telemetry run in esp_link_task as required by the design.
 */
#include "esp_link.h"
#include "esp_link_protocol.h"
#include "esp_activity.h"
#include "fpga.h"
#include "battery.h"
#include "lcd.h"
#include "meter_data.h"
#include "input_handler.h"
#include "scope_state.h"
#include "scope_cal.h"
#include "scope_timebase.h"
#include "ui.h"
#include "FreeRTOS.h"
#include "task.h"
#include "semphr.h"
#include "at32f403a_407.h"
#include "at32f403a_407_crm.h"
#include "at32f403a_407_gpio.h"
#include "at32f403a_407_misc.h"
#include "at32f403a_407_usart.h"
#include "at32f403a_407_wdt.h"
#include <string.h>

#ifdef ESP_LINK_BRIDGE
#define RX_RING_SIZE       1024u
#define TX_RING_SIZE        512u
#else
#define RX_RING_SIZE       2048u
#define TX_RING_SIZE       1024u
#endif
#define LINK_TASK_STACK     512u
#define LINK_HEARTBEAT_MS  1000u
#define TX_MAX_PAYLOAD      192u
#ifdef ESP_LINK_BRIDGE
#define ESP_LINK_SOFT_TX       0u
#define LINK_UART USART2
#define LINK_IRQ USART2_IRQn
#else
#define ESP_LINK_SOFT_TX       1u
#define LINK_UART UART8
#define LINK_IRQ UART8_IRQn
#endif
#define SOFT_TX_BIT_CYCLES ((240000000u + ESP_LINK_BAUD / 2u) / ESP_LINK_BAUD)
#define TX_DECODED_SIZE    (OSLP_HEADER_SIZE + TX_MAX_PAYLOAD + OSLP_CRC_SIZE)
#define TX_ENCODED_SIZE    (TX_DECODED_SIZE + (TX_DECODED_SIZE / 254u) + 2u)
#define UART_CTRL1_TDBEIEN (1u << 7) /* AT32 CTRL1 transmit-buffer-empty IRQ */

typedef struct {
    uint8_t rx_ring[RX_RING_SIZE];
    uint8_t tx_ring[TX_RING_SIZE];
    /* COBS decoding is safe in-place because the decoder's write cursor never
     * overtakes its read cursor. This retains 1024-byte decoded RX frames
     * without paying for a second 1 KiB buffer. TX remains separate because
     * the input task can transmit while the link task dispatches RX. Phase-1
     * host responses top out below 192 payload bytes, so outbound staging does
     * not inherit the much larger peer-to-host UI frame allowance. */
    uint8_t rx_encoded[OSLP_MAX_ENCODED_FRAME];
    uint8_t tx_encoded[TX_ENCODED_SIZE];
    uint8_t tx_decoded[TX_DECODED_SIZE];
    volatile uint16_t rx_head, rx_tail, tx_head, tx_tail;
    uint16_t encoded_len;
    uint16_t tx_sequence;
    uint16_t next_transaction;
    uint16_t session_id;
    uint32_t peer_nonce;
    uint32_t local_nonce;
    uint32_t last_rx_ms;
    uint32_t last_tx_ms;
    uint32_t meter_last_revision;
    uint16_t meter_period_ms;
    uint16_t battery_period_ms;
    uint16_t mode_period_ms;
    uint32_t meter_last_ms, battery_last_ms, mode_last_ms;
    uint16_t detail_period_ms, scope_period_ms;
    uint32_t detail_last_ms, scope_last_ms, telemetry_lease_ms;
    uint32_t scope_blocked_generation;
    /* 128 raw samples/channel, stride 8 across the 1024-byte acquisition.
     * One immutable preview spans two UART envelopes. This bounded 256-byte
     * cache avoids adding a full framebuffer or exhausting the AT32 RTOS heap. */
    uint8_t scope_samples[256], scope_meta[30], scope_part;
    bool discard_encoded;
    bool available;
    volatile bool bus_quiesce_ack;
    volatile uint16_t bus_quiesce_transaction;
    volatile uint16_t meter_transaction;
    volatile uint8_t meter_pending, meter_done, meter_error;
    SemaphoreHandle_t tx_mutex;
    esp_link_stats_t stats;
} link_state_t;

static link_state_t *s;
/* This tiny fallback survives a failed dynamic allocation so diagnostics can
 * still distinguish an absent ESP board from exhausted RTOS heap. Large rings
 * and workspaces deliberately remain off .bss: the linker leaves only about
 * two KiB of unreserved AT32 SRAM in the normal build. */
static esp_link_stats_t failed_stats;
#ifdef ESP_LINK_BRIDGE
static uint16_t boot_meter_transaction;
#endif

#if ESP_LINK_SOFT_TX
/* Bench source of truth, 2C53T-V1.4: UART8 receives correctly on PE0, while
 * its TDBE/TDC state advances without any waveform reaching PE1. XMC_UB keeps
 * priority at the pad even with bank 0 fully deconfigured. Drive only TX as
 * GPIO 8N1. One byte masks interrupts for about 10.9 us at 921600 baud;
 * interrupts are restored between bytes and UART permits inter-byte gaps. */
static inline void soft_tx_wait_until(uint32_t deadline)
{
    while((int32_t)(DWT->CYCCNT-deadline)<0) __asm volatile("nop");
}

static void soft_tx_byte(uint8_t value)
{
    uint32_t saved_primask=__get_PRIMASK();
    __disable_irq();
    uint32_t deadline=DWT->CYCCNT;
    GPIOE->clr=GPIO_PINS_1; /* start */
    deadline+=SOFT_TX_BIT_CYCLES;soft_tx_wait_until(deadline);
    for(uint8_t bit=0;bit<8u;bit++) {
        if(value&(1u<<bit)) GPIOE->scr=GPIO_PINS_1;
        else GPIOE->clr=GPIO_PINS_1;
        deadline+=SOFT_TX_BIT_CYCLES;soft_tx_wait_until(deadline);
    }
    GPIOE->scr=GPIO_PINS_1; /* stop / idle */
    deadline+=SOFT_TX_BIT_CYCLES;soft_tx_wait_until(deadline);
    if(!saved_primask)__enable_irq();
}

static void soft_tx_service(void)
{
    if(!s||!lcd_uart_bus_uart_active()||!lcd_uart_bus_tx_allowed())return;
    while(s->tx_tail!=s->tx_head&&lcd_uart_bus_uart_active()&&
          lcd_uart_bus_tx_allowed()) {
        uint16_t tail=s->tx_tail;
        soft_tx_byte(s->tx_ring[tail&(TX_RING_SIZE-1u)]);
        s->tx_tail=(uint16_t)(tail+1u);
        s->stats.tx_bytes++;
    }
}
#endif

static uint16_t ring_used(uint16_t head, uint16_t tail)
{
    return (uint16_t)(head - tail);
}

static uint32_t now_ms(void)
{
    return (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
}

static void put16(uint8_t *p, uint16_t v) { p[0]=(uint8_t)v; p[1]=(uint8_t)(v>>8); }
static void put32(uint8_t *p, uint32_t v) { p[0]=(uint8_t)v; p[1]=(uint8_t)(v>>8); p[2]=(uint8_t)(v>>16); p[3]=(uint8_t)(v>>24); }
static uint16_t get16(const uint8_t *p) { return (uint16_t)p[0] | ((uint16_t)p[1]<<8); }
static uint32_t get32(const uint8_t *p) { return (uint32_t)p[0] | ((uint32_t)p[1]<<8) | ((uint32_t)p[2]<<16) | ((uint32_t)p[3]<<24); }

#ifndef ESP_LINK_BRIDGE
void UART8_IRQHandler(void) { esp_link_uart_irq(); }
#endif

void esp_link_uart_irq(void)
{
    uint32_t sts = LINK_UART->sts;
    if (!s) {
        if (sts & (USART_RDBF_FLAG | USART_ROERR_FLAG)) (void)LINK_UART->dt;
        LINK_UART->ctrl1 &= ~UART_CTRL1_TDBEIEN;
        return;
    }
    if (!lcd_uart_bus_uart_active()) return;
    s->stats.irq_count++;
    s->stats.last_status = sts;
    if (sts & (USART_PERR_FLAG | USART_FERR_FLAG | USART_NERR_FLAG | USART_ROERR_FLAG)) {
        s->stats.error_irq_count++;
        (void)LINK_UART->dt; /* AT32 clears receive error state by STS then DT read. */
        s->stats.framing_errors++;
    } else if (sts & USART_RDBF_FLAG) {
        s->stats.rx_irq_count++;
        uint8_t b = (uint8_t)LINK_UART->dt;
        lcd_uart_bus_note_rx_activity();
        uint16_t head = s->rx_head;
        if (ring_used(head, s->rx_tail) < RX_RING_SIZE) {
            s->rx_ring[head & (RX_RING_SIZE - 1u)] = b;
            s->rx_head = (uint16_t)(head + 1u);
            s->stats.rx_bytes++;
        } else {
            s->stats.rx_overflow++;
        }
    }
#if !ESP_LINK_SOFT_TX
    if ((LINK_UART->ctrl1 & UART_CTRL1_TDBEIEN) && (sts & USART_TDBE_FLAG)) {
        s->stats.tx_irq_count++;
        uint16_t tail = s->tx_tail;
        if (tail != s->tx_head) {
            LINK_UART->dt = s->tx_ring[tail & (TX_RING_SIZE - 1u)];
            s->tx_tail = (uint16_t)(tail + 1u);
            s->stats.tx_bytes++;
        } else {
            LINK_UART->ctrl1 &= ~UART_CTRL1_TDBEIEN;
        }
    }
#endif
}

static bool tx_enqueue(const uint8_t *data, uint16_t length, bool force)
{
    if (!s || length > TX_RING_SIZE) return false;
    taskENTER_CRITICAL();
    uint16_t head = s->tx_head;
    if ((uint16_t)(TX_RING_SIZE - ring_used(head, s->tx_tail)) < length) {
        s->stats.tx_overflow++;
        taskEXIT_CRITICAL();
        return false;
    }
    for (uint16_t i = 0; i < length; ++i)
        s->tx_ring[(head + i) & (TX_RING_SIZE - 1u)] = data[i];
    s->tx_head = (uint16_t)(head + length);
#if !ESP_LINK_SOFT_TX
    if (force || lcd_uart_bus_tx_allowed()) LINK_UART->ctrl1 |= UART_CTRL1_TDBEIEN;
#else
    (void)force;
#endif
    taskEXIT_CRITICAL();
    return true;
}

static bool send_frame_internal(uint8_t kind, uint8_t service, uint8_t opcode,
                                uint16_t transaction, const uint8_t *payload,
                                uint16_t length, bool bus_control)
{
    const bool terminal = kind==OSLP_KIND_RESPONSE || kind==OSLP_KIND_ACK ||
                          kind==OSLP_KIND_ERROR;
    const bool frozen_ok = bus_control || terminal;
    if (!s || !s->tx_mutex || !lcd_uart_bus_uart_active() ||
        length > TX_MAX_PAYLOAD ||
        (!frozen_ok && !lcd_uart_bus_tx_allowed()) ||
        xSemaphoreTake(s->tx_mutex, pdMS_TO_TICKS(50)) != pdTRUE) return false;
    /* A producer can have been waiting on tx_mutex while an LCD owner raised
     * the pending flag. Recheck under serialization so QUIESCE is guaranteed
     * to be the final ordinary AT32->ESP frame. */
    if (!frozen_ok && !lcd_uart_bus_tx_allowed()) {
        xSemaphoreGive(s->tx_mutex);
        return false;
    }
    oslp_frame_t f = {
        .kind=kind, .flags=OSLP_FLAG_RAW, .service=service, .opcode=opcode,
        .session_id=s->session_id, .transaction_id=transaction,
        .sequence=++s->tx_sequence, .payload_length=length, .payload=payload,
    };
    size_t raw = oslp_frame_build(&f, s->tx_decoded, sizeof(s->tx_decoded));
    size_t enc = oslp_cobs_encode(s->tx_decoded, raw, s->tx_encoded,
                                  sizeof(s->tx_encoded) - 1u);
    if (!raw || !enc) { xSemaphoreGive(s->tx_mutex); return false; }
    s->tx_encoded[enc++] = 0;
#ifdef ESP_LINK_BRIDGE
    /* Reserve an entire meter envelope. Nonessential producers may retry;
     * they must not fill the ring and prevent the next native command. */
    if(service!=OSLP_SERVICE_METER_BRIDGE &&
       TX_RING_SIZE-ring_used(s->tx_head,s->tx_tail)<enc+40u) {
        xSemaphoreGive(s->tx_mutex);return false;
    }
#endif
    if (!tx_enqueue(s->tx_encoded, (uint16_t)enc, frozen_ok)) { xSemaphoreGive(s->tx_mutex); return false; }
    s->last_tx_ms = now_ms();
    xSemaphoreGive(s->tx_mutex);
    return true;
}

static bool send_frame(uint8_t kind, uint8_t service, uint8_t opcode,
                       uint16_t transaction, const uint8_t *payload,
                       uint16_t length)
{
    return send_frame_internal(kind, service, opcode, transaction, payload,
                               length, false);
}

static uint16_t new_transaction(void)
{
    uint16_t transaction;
    taskENTER_CRITICAL();
    if (++s->next_transaction == 0) ++s->next_transaction;
    transaction = s->next_transaction;
    taskEXIT_CRITICAL();
    return transaction;
}

static void reply_error(const oslp_frame_t *in, uint8_t error)
{
    uint8_t p[] = { OSLP_RAW_ABI_VERSION, error };
    (void)send_frame(OSLP_KIND_ERROR, in->service, in->opcode,
                     in->transaction_id, p, sizeof(p));
}

static void handle_hello(const oslp_frame_t *f)
{
    /* ESP phase-1 HELLO raw v0:
     *   u8 abi,u8 protocol_major,u8 protocol_minor,u16 max_frame,
     *   u32 boot_nonce,u8 service_count,u8 heartbeat_s,u8 max_outstanding.
     * The negotiated session is authoritative in the response envelope. */
    if (f->payload_length < 12 || f->payload[0] != OSLP_RAW_ABI_VERSION ||
        f->payload[1] != OSLP_VERSION_MAJOR) {
        reply_error(f, 1); return;
    }
    uint32_t peer = get32(f->payload + 5);
    const bool new_session = peer != s->peer_nonce || !s->session_id;
    if (new_session) {
        s->peer_nonce = peer;
        uint32_t mixed = oslp_crc32c((const uint8_t *)&peer, sizeof(peer)) ^ s->local_nonce;
        s->session_id = (uint16_t)(mixed ^ (mixed >> 16));
        if (!s->session_id) s->session_id = 1;
        s->tx_sequence = 0;
        s->meter_period_ms=s->battery_period_ms=s->mode_period_ms=0;
        s->detail_period_ms=s->scope_period_ms=0;
        s->scope_part=2;
        s->stats.reconnects++;
        esp_activity_link_ready();
    }
    s->available = true;
    s->last_rx_ms = now_ms();
    uint8_t p[12] = { OSLP_RAW_ABI_VERSION, OSLP_VERSION_MAJOR,
                      OSLP_VERSION_MINOR, 0, 0, 0, 0, 0, 0, 4, 1, 4 };
    put16(p+3,OSLP_MAX_DECODED_FRAME); put32(p+5,s->local_nonce);
    (void)send_frame(OSLP_KIND_RESPONSE, OSLP_SERVICE_CORE, OSLP_CORE_HELLO,
                     f->transaction_id, p, sizeof(p));
}

static void send_catalog(uint16_t transaction)
{
    /* CATALOG response raw v0: u8 abi,u8 count, then repeated
     * u16 id,u8 access(read=1,subscribe=2),u16 min_period_ms,u8 path_len,path. */
    static const struct { uint16_t id, period; uint8_t access; const char *path; } d[] = {
        { OSLP_RESOURCE_UPTIME, 1000, 1, "system/uptime" },
        { OSLP_RESOURCE_BATTERY, 1000, 3, "system/battery" },
        { OSLP_RESOURCE_DEVICE_MODE, 100, 3, "device/mode" },
        { OSLP_RESOURCE_METER_READING, 250, 3, "meter/reading" },
        { OSLP_RESOURCE_METER_DETAIL, 250, 3, "meter/detail" },
        { OSLP_RESOURCE_SCOPE_PREVIEW, 200, 2, "scope/preview" },
    };
    uint8_t p[192]; uint16_t n=0; p[n++]=OSLP_RAW_ABI_VERSION; p[n++]=(uint8_t)(sizeof(d)/sizeof(d[0]));
    for (unsigned i=0;i<sizeof(d)/sizeof(d[0]);++i) {
        size_t l=strlen(d[i].path); put16(p+n,d[i].id); n+=2; p[n++]=d[i].access;
        put16(p+n,d[i].period); n+=2; p[n++]=(uint8_t)l; memcpy(p+n,d[i].path,l); n+=(uint16_t)l;
    }
    (void)send_frame(OSLP_KIND_RESPONSE,OSLP_SERVICE_DEVICE,OSLP_DEVICE_CATALOG,transaction,p,n);
}

static void send_snapshot(uint16_t resource, uint16_t transaction, bool event)
{
    /* General snapshots use an AT32-rich v0 prefix:
     * u8 abi,u16 resource,u32 revision,u32 timestamp_ms,u8 quality,u8 type.
     * type 1=u32, 2=battery(percent,u16 mV,flags), 3=u8, 4=meter struct. */
    uint8_t p[96]; uint16_t n=0; p[n++]=OSLP_RAW_ABI_VERSION; put16(p+n,resource); n+=2;
    uint32_t revision=now_ms();
    if(resource==OSLP_RESOURCE_METER_READING) revision=meter_reading.update_count;
    put32(p+n,revision); n+=4; put32(p+n,now_ms()); n+=4; p[n++]=0; /* VALID */
    if(resource==OSLP_RESOURCE_UPTIME) { p[n++]=1; put32(p+n,uptime_seconds); n+=4; }
    else if(resource==OSLP_RESOURCE_BATTERY) { p[n++]=2; p[n++]=battery_percent(); put16(p+n,battery_read_mv()); n+=2; p[n++]=battery_is_charging(); }
    else if(resource==OSLP_RESOURCE_DEVICE_MODE) { p[n++]=3; p[n++]=(uint8_t)current_mode; }
    else if(resource==OSLP_RESOURCE_METER_DETAIL) {
        meter_reading_t m; memset(&m,0,sizeof(m)); bool ok=meter_data_snapshot(&m);
        /* v0 detail: prefix through timestamp; quality, submode, result,
         * reject, flags, f32 value, NUL-padded display[16], unit[8]. Keep the
         * parser's actual unit/OL/rejection, not a guessed unit from submode. */
        put32(p+3,m.update_count);
        p[11]=(!ok||!m.valid)?6u:(m.result_class==METER_RESULT_OVERLOAD?2u:(m.reject_reason?5u:0u));
        p[12]=m.submode;p[13]=(uint8_t)m.result_class;p[14]=m.reject_reason;
        p[15]=(m.is_hold?1u:0u)|(m.is_auto_range?2u:0u)|(m.is_ac?4u:0u);
        memcpy(p+16,&m.value,4);memset(p+20,0,24);
        memcpy(p+20,m.display_str,15);
        if(m.unit_suffix)strncpy((char*)p+36,m.unit_suffix,7);
        n=44;
    }
    else if(resource==OSLP_RESOURCE_METER_READING) {
        /* ESP MeasurementService draft v0 is deliberately the minimal common
         * denominator: v,id,revision,f32 value,u8 unit,u8 quality. Quality is
         * never silently coerced to VALID when the AT32 refuses a reading. */
        meter_reading_t m; memset(&m,0,sizeof(m)); bool ok=meter_data_snapshot(&m);
        n=0;p[n++]=OSLP_RAW_ABI_VERSION;put16(p+n,resource);n+=2;
        put32(p+n,ok?m.update_count:meter_reading.update_count);n+=4;
        float value=ok?m.value:0.0f;memcpy(p+n,&value,4);n+=4;
        p[n++]=m.submode;
        p[n++]=(!ok||!m.valid)?6u:(m.result_class==METER_RESULT_OVERLOAD?2u:(m.reject_reason?5u:0u));
    } else return;
    (void)send_frame(event?OSLP_KIND_EVENT:OSLP_KIND_RESPONSE,OSLP_SERVICE_DEVICE,
                     OSLP_DEVICE_SNAPSHOT,transaction,p,n);
}

static void handle_device(const oslp_frame_t *f)
{
    if(f->kind!=OSLP_KIND_REQUEST)return;
    if(f->opcode==OSLP_DEVICE_SET_MODE) {
        if(f->payload_length!=2 || f->payload[0]!=0 || f->payload[1]>=MODE_COUNT) {reply_error(f,1);return;}
        if(!input_request_mode((device_mode_t)f->payload[1])) {reply_error(f,3);return;}
        /* ACK means queued, not already applied. DEVICE_MODE telemetry is the
         * authoritative confirmation after the input task runs the transition. */
        (void)send_frame(OSLP_KIND_ACK,OSLP_SERVICE_DEVICE,f->opcode,f->transaction_id,f->payload,2);return;
    }
    if (f->opcode==OSLP_DEVICE_CATALOG) { send_catalog(f->transaction_id); return; }
    if (f->payload_length<3 || f->payload[0]!=OSLP_RAW_ABI_VERSION) { reply_error(f,1); return; }
    uint16_t resource=get16(f->payload+1);
    if(f->opcode==OSLP_DEVICE_SNAPSHOT) {
        if(resource!=OSLP_RESOURCE_UPTIME && resource!=OSLP_RESOURCE_BATTERY &&
           resource!=OSLP_RESOURCE_DEVICE_MODE && resource!=OSLP_RESOURCE_METER_READING &&
           resource!=OSLP_RESOURCE_METER_DETAIL){reply_error(f,2);return;}
        send_snapshot(resource,f->transaction_id,false); return;
    }
    if(f->opcode==OSLP_DEVICE_SUBSCRIBE) {
        if(f->payload_length<8){reply_error(f,1);return;}
        uint32_t period_us=get32(f->payload+3);
        uint32_t requested=period_us/1000u;
        uint16_t period=(uint16_t)(requested>10000u?10000u:requested);
        if(resource==OSLP_RESOURCE_BATTERY){if(period<1000)period=1000;s->battery_period_ms=period;}
        else if(resource==OSLP_RESOURCE_DEVICE_MODE){if(period<100)period=100;s->mode_period_ms=period;}
        else if(resource==OSLP_RESOURCE_METER_READING){if(period<250)period=250;s->meter_period_ms=period;}
        else if(resource==OSLP_RESOURCE_METER_DETAIL){if(period<250)period=250;s->detail_period_ms=period;}
        else if(resource==OSLP_RESOURCE_SCOPE_PREVIEW){if(period<200)period=200;s->scope_period_ms=period;}
        else {reply_error(f,2);return;}
        s->telemetry_lease_ms=now_ms();
        uint8_t p[5]={OSLP_RAW_ABI_VERSION,0,0,0,0}; put16(p+1,resource); put16(p+3,period);
        (void)send_frame(OSLP_KIND_RESPONSE,OSLP_SERVICE_DEVICE,f->opcode,f->transaction_id,p,sizeof(p)); return;
    }
    if(f->opcode==OSLP_DEVICE_UNSUBSCRIBE) {
        if(resource==OSLP_RESOURCE_BATTERY)s->battery_period_ms=0;
        else if(resource==OSLP_RESOURCE_DEVICE_MODE)s->mode_period_ms=0;
        else if(resource==OSLP_RESOURCE_METER_READING)s->meter_period_ms=0;
        else if(resource==OSLP_RESOURCE_METER_DETAIL)s->detail_period_ms=0;
        else if(resource==OSLP_RESOURCE_SCOPE_PREVIEW){s->scope_period_ms=0;s->scope_part=2;}
        else {reply_error(f,2);return;}
        uint8_t p[3]={OSLP_RAW_ABI_VERSION,0,0};put16(p+1,resource);
        (void)send_frame(OSLP_KIND_RESPONSE,OSLP_SERVICE_DEVICE,f->opcode,f->transaction_id,p,sizeof(p));return;
    }
    reply_error(f,2);
}

static void dispatch(const oslp_frame_t *f)
{
#ifdef ESP_LINK_BRIDGE
    /* Physical meter forwarding must work before Wi-Fi/apps/HELLO, and survive
     * application-session expiry. Only this bounded service bypasses session. */
    if (f->service==OSLP_SERVICE_METER_BRIDGE) {
        if (f->opcode==OSLP_METER_TX_DONE &&
            f->transaction_id==s->meter_transaction && s->meter_pending &&
            f->payload_length==0 &&
            (f->kind==OSLP_KIND_ACK || f->kind==OSLP_KIND_ERROR)) {
            s->meter_error=(f->kind==OSLP_KIND_ACK)?0u:1u;
            /* ESP sends TX_DONE before wrapping bytes received during that
             * command. Release the original parser's TX-busy guard here,
             * before the next meter RX envelope in the same ring is parsed. */
            if(!s->meter_error)fpga.tx_index=FPGA_TX_FRAME_SIZE;
            s->meter_done=1;
        } else if (f->opcode==OSLP_METER_RX && f->kind==OSLP_KIND_STREAM &&
                   f->payload_length<=64u) {
            for (uint16_t i=0;i<f->payload_length;i++)
                fpga_uart_receive_byte(f->payload[i],false);
            s->stats.meter_rx_bytes+=f->payload_length;
        }
        return;
    }
#endif
    if(f->service==OSLP_SERVICE_CORE && f->opcode==OSLP_CORE_HELLO){handle_hello(f);return;}
    if(!s->session_id || f->session_id!=s->session_id)return;
    s->last_rx_ms=now_ms();
    if(f->service==OSLP_SERVICE_CORE && f->opcode==OSLP_CORE_HEARTBEAT){
        if(f->kind==OSLP_KIND_REQUEST)
            (void)send_frame(OSLP_KIND_RESPONSE,OSLP_SERVICE_CORE,OSLP_CORE_HEARTBEAT,f->transaction_id,NULL,0);
        return;
    }
    if(f->service==OSLP_SERVICE_CORE && f->opcode==OSLP_CORE_BUS_QUIESCE &&
       f->kind==OSLP_KIND_RESPONSE &&
       f->transaction_id==s->bus_quiesce_transaction &&
       f->payload_length>=1 && f->payload[0]==OSLP_RAW_ABI_VERSION) {
        s->bus_quiesce_ack=true;
        return;
    }
    if(f->service==OSLP_SERVICE_DEVICE){handle_device(f);return;}
    if(f->service==OSLP_SERVICE_APP && f->opcode==OSLP_APP_LIST){esp_activity_receive_app_list(f->payload,f->payload_length);return;}
    if(f->service==OSLP_SERVICE_APP && f->opcode==OSLP_APP_START){esp_activity_receive_start(f->payload,f->payload_length);return;}
    if(f->service==OSLP_SERVICE_APP && f->opcode==OSLP_APP_STOP && f->kind==OSLP_KIND_REQUEST &&
       f->payload_length==5 && f->payload[0]==OSLP_RAW_ABI_VERSION) {
        if(get32(f->payload+1)==esp_activity_token())esp_activity_stop_local();
        (void)send_frame(OSLP_KIND_ACK,OSLP_SERVICE_APP,OSLP_APP_STOP,f->transaction_id,NULL,0);return;
    }
    if(f->service==OSLP_SERVICE_UI && f->opcode==OSLP_UI_ACTIVITY_STATE){
        esp_activity_receive_state(f->payload,f->payload_length,
                                   f->kind==OSLP_KIND_REQUEST?f->transaction_id:0);return;
    }
}

static void receive_byte(uint8_t b)
{
    if(b!=0){
        if(!s->discard_encoded && s->encoded_len<OSLP_MAX_ENCODED_FRAME)
            s->rx_encoded[s->encoded_len++]=b;
        else s->discard_encoded=true;
        return;
    }
    if(s->discard_encoded || s->encoded_len==0){s->discard_encoded=false;s->encoded_len=0;return;}
    size_t n=oslp_cobs_decode(s->rx_encoded,s->encoded_len,s->rx_encoded,
                              OSLP_MAX_DECODED_FRAME);
    s->encoded_len=0;
    oslp_frame_t f;
    if(!n || !oslp_frame_parse(s->rx_encoded,n,&f)){s->stats.crc_or_frame_errors++;return;}
    s->stats.valid_frames++;dispatch(&f);
}

static bool capture_scope(uint32_t now)
{
    if(current_mode!=MODE_OSCILLOSCOPE)return false;
    const volatile uint8_t *a=fpga_get_ch1_buf(), *b=fpga_get_ch2_buf();
    if(!a || !b || !(fpga_data_ready() || scope_acquisition_ready()))return false;
    uint32_t gen=fpga_acq_frame_generation();
    if(!gen || (gen&1u) || gen==s->scope_blocked_generation)return false;
    scope_state_t config=*scope_state_get();
    _Static_assert(FPGA_ADC_BUF_SIZE==1024u,"preview stride assumes a 1024-point record");
    for(unsigned i=0;i<128;i++){s->scope_samples[i]=a[i*8];s->scope_samples[128+i]=b[i*8];}
    if(gen!=fpga_acq_frame_generation() || memcmp(&config,scope_state_get(),sizeof(config)))return false;
    uint8_t *p=s->scope_meta;memset(p,0,30);put16(p+1,OSLP_RESOURCE_SCOPE_PREVIEW);
    put32(p+3,gen);put32(p+7,now);
    p[11]=1u|(config.running?2u:0u)|(config.ch1.enabled?4u:0u)|(config.ch2.enabled?8u:0u)|
          (fpga_acq_record_time_ordered()?16u:0u);
    p[13]=config.timebase_idx;p[14]=8;
    float fs=scope_timebase_sample_rate(config.timebase_idx);
    float v1=scope_cal_volts_per_count(1,config.ch1.vdiv_idx),v2=scope_cal_volts_per_count(2,config.ch2.vdiv_idx);
    memcpy(p+15,&fs,4);memcpy(p+19,&v1,4);memcpy(p+23,&v2,4);
    p[27]=(uint8_t)scope_cal_get_tier(1,config.ch1.vdiv_idx);
    p[28]=(uint8_t)scope_cal_get_tier(2,config.ch2.vdiv_idx);
    p[29]=(uint8_t)scope_timebase_get_tier(config.timebase_idx);
    if(gen!=fpga_acq_frame_generation() || memcmp(&config,scope_state_get(),sizeof(config)))return false;
    s->scope_part=0;
    return true;
}

static void telemetry_service(uint32_t now)
{
    /* Subscriptions are leases. A crashed browser/ESP cannot leave an
     * unattended stream competing with the transparent meter bridge forever. */
    if(now-s->telemetry_lease_ms>10000u) {
        s->battery_period_ms=s->mode_period_ms=s->meter_period_ms=0;
        s->detail_period_ms=s->scope_period_ms=0;s->scope_part=2;return;
    }
    if(s->battery_period_ms && now-s->battery_last_ms>=s->battery_period_ms){s->battery_last_ms=now;send_snapshot(OSLP_RESOURCE_BATTERY,0,true);}
    if(s->mode_period_ms && now-s->mode_last_ms>=s->mode_period_ms){s->mode_last_ms=now;send_snapshot(OSLP_RESOURCE_DEVICE_MODE,0,true);}
    if(s->meter_period_ms && meter_reading.update_count!=s->meter_last_revision && now-s->meter_last_ms>=s->meter_period_ms){
        s->meter_last_ms=now;s->meter_last_revision=meter_reading.update_count;send_snapshot(OSLP_RESOURCE_METER_READING,0,true);
    }
    if(s->detail_period_ms && current_mode==MODE_MULTIMETER && now-s->detail_last_ms>=s->detail_period_ms) {
        s->detail_last_ms=now;send_snapshot(OSLP_RESOURCE_METER_DETAIL,0,true);
    }
    if(current_mode!=MODE_OSCILLOSCOPE) {
        s->scope_blocked_generation=fpga_acq_frame_generation();s->scope_part=2;return;
    }
    if(!s->scope_period_ms){s->scope_part=2;return;}
    if(s->scope_part>=2 && now-s->scope_last_ms>=s->scope_period_ms) {
        s->scope_last_ms=now;(void)capture_scope(now);
    }
    if(s->scope_part<2) {
        uint8_t p[158];memcpy(p,s->scope_meta,30);p[12]=s->scope_part;
        memcpy(p+30,s->scope_samples+s->scope_part*64,64);
        memcpy(p+94,s->scope_samples+128+s->scope_part*64,64);
        /* One chunk per task tick; retry a full TX ring, never block or evict
         * a meter command. Both chunks share an immutable ADC generation. */
        if(send_frame(OSLP_KIND_EVENT,OSLP_SERVICE_DEVICE,OSLP_DEVICE_SNAPSHOT,0,p,sizeof(p)))++s->scope_part;
    }
}

static void link_task(void *arg)
{
    (void)arg;
    for(;;){
        /* A sticky RDBF with irq_count==0 identifies NVIC/vector trouble;
         * no RDBF despite a measured waveform identifies pin/mux/peripheral
         * selection instead.  A 32-bit diagnostic sample is atomic on M4. */
        if(lcd_uart_bus_uart_active()) {
            s->stats.last_status=LINK_UART->sts;
            s->stats.last_control1=LINK_UART->ctrl1;
            s->stats.baud_divider=(uint16_t)LINK_UART->baudr;
        }
#ifndef ESP_LINK_BRIDGE
        s->stats.uart8_gmux=(uint8_t)IOMUX->remap8_bit.uart8_gmux;
        s->stats.pe01_config=(uint8_t)GPIOE->cfglr;
        s->stats.pe0_level=(GPIOE->idt & GPIO_PINS_0) ? 1u : 0u;
#else
        s->stats.pe01_config=(uint8_t)(GPIOA->cfglr>>8);
        s->stats.pe0_level=(GPIOA->idt & GPIO_PINS_3)?1u:0u;
#endif
        /* A frame queued while LCD owned XMC is drained after release. */
#if ESP_LINK_SOFT_TX
        soft_tx_service();
#else
        if(lcd_uart_bus_uart_active() && lcd_uart_bus_tx_allowed() &&
           s->tx_tail!=s->tx_head)
            LINK_UART->ctrl1 |= UART_CTRL1_TDBEIEN;
#endif
        while(s->rx_tail!=s->rx_head){uint8_t b=s->rx_ring[s->rx_tail&(RX_RING_SIZE-1u)];s->rx_tail++;receive_byte(b);}
        uint32_t now=now_ms();
        if(current_mode!=MODE_OSCILLOSCOPE)
            s->scope_blocked_generation=fpga_acq_frame_generation();
        if(s->available && now-s->last_rx_ms>3u*LINK_HEARTBEAT_MS){
            s->available=false;s->session_id=0;s->meter_period_ms=s->battery_period_ms=s->mode_period_ms=0;
            s->detail_period_ms=s->scope_period_ms=0;s->scope_part=2;esp_activity_link_lost();
        } else if(s->available && now-s->last_tx_ms>=LINK_HEARTBEAT_MS) {
            (void)send_frame(OSLP_KIND_EVENT,OSLP_SERVICE_CORE,OSLP_CORE_HEARTBEAT,0,NULL,0);
        } else if(!s->available && now-s->last_tx_ms>=LINK_HEARTBEAT_MS) {
            /* Pre-session liveness beacon.  It deliberately carries session
             * zero and cannot establish a session; its only job is to prove
             * the AT32 UART8 TX route independently while waiting for HELLO. */
            (void)send_frame(OSLP_KIND_EVENT,OSLP_SERVICE_CORE,OSLP_CORE_HEARTBEAT,0,NULL,0);
        }
        if(s->available)telemetry_service(now);
        vTaskDelay(pdMS_TO_TICKS(5));
    }
}

bool esp_link_bus_quiesce(uint32_t timeout_ms)
{
#ifdef ESP_LINK_BRIDGE
    (void)timeout_ms;
    return true; /* USART2 has no XMC pad conflict. */
#else
    if(!s || !s->available || !s->session_id ||
       !lcd_uart_bus_uart_active()) return false;

    const uint16_t transaction=new_transaction();
    /* v, maximum XMC hold in milliseconds. The peer's recovery lease must be
     * longer than a full native/LVGL render batch, not a fixed local guess. */
    const uint8_t payload[3]={OSLP_RAW_ABI_VERSION,0xD0u,0x07u}; /* 2000 ms */
    s->bus_quiesce_ack=false;
    s->bus_quiesce_transaction=transaction;
    if(!send_frame_internal(OSLP_KIND_REQUEST,OSLP_SERVICE_CORE,
                            OSLP_CORE_BUS_QUIESCE,transaction,payload,
                            sizeof(payload),true)) return false;

    TickType_t start=xTaskGetTickCount();
    TickType_t timeout=pdMS_TO_TICKS(timeout_ms);
    if(timeout==0) timeout=1;
    while(!s->bus_quiesce_ack &&
          (TickType_t)(xTaskGetTickCount()-start)<timeout)
        vTaskDelay(1);
    if(!s->bus_quiesce_ack) goto link_down;

    /* The peer flushed its ACK before becoming silent. Its receipt proves the
     * ESP direction is idle; TDC additionally proves the local final stop bit
     * has left PE1 before that pad is changed from UART MUX to GPIO. */
    /* Terminal responses to peer requests already in flight remain legal
     * during the freeze. Drain all of them after ACK before changing PE1. */
    start=xTaskGetTickCount();
    timeout=pdMS_TO_TICKS(20u);
    while((s->tx_tail!=s->tx_head || !(UART8->sts & USART_TDC_FLAG)) &&
          (TickType_t)(xTaskGetTickCount()-start)<timeout)
        vTaskDelay(1);
    if(s->tx_tail==s->tx_head && s->rx_tail==s->rx_head &&
       s->encoded_len==0 && (UART8->sts & USART_TDC_FLAG)) return true;

link_down:
    /* Native display wins over a broken/old peer. Stop adding bytes, allow the
     * current UART character to finish, invalidate all stream/session state,
     * and take an offline XMC grant. The ESP lease/HELLO loop recovers later. */
    taskENTER_CRITICAL();
    UART8->ctrl1 &= ~UART_CTRL1_TDBEIEN;
    s->tx_tail=s->tx_head;
    s->rx_tail=s->rx_head;
    s->encoded_len=0;
    s->discard_encoded=false;
    s->available=false;
    s->session_id=0;
    s->meter_period_ms=s->battery_period_ms=s->mode_period_ms=0;
    taskEXIT_CRITICAL();
    start=xTaskGetTickCount();
    while(!(UART8->sts & USART_TDC_FLAG) &&
          (TickType_t)(xTaskGetTickCount()-start)<pdMS_TO_TICKS(2u))
        taskYIELD();
    esp_activity_link_lost();
    return false;
#endif
}

bool esp_link_bus_resume(void)
{
#ifdef ESP_LINK_BRIDGE
    return true;
#else
    if(!s || !s->session_id || !lcd_uart_bus_uart_active()) return false;
    const uint8_t payload[1]={OSLP_RAW_ABI_VERSION};
    bool ok=send_frame_internal(OSLP_KIND_EVENT,OSLP_SERVICE_CORE,
                                OSLP_CORE_BUS_RESUME,0,payload,
                                sizeof(payload),true);
    s->bus_quiesce_ack=false;
    if(!ok) {
        taskENTER_CRITICAL();
        s->available=false;s->session_id=0;
        s->meter_period_ms=s->battery_period_ms=s->mode_period_ms=0;
        taskEXIT_CRITICAL();
        esp_activity_link_lost();
    }
    return ok;
#endif
}

bool esp_link_meter_send(const uint8_t *frame, size_t length)
{
#ifdef ESP_LINK_BRIDGE
    if (!frame || length!=FPGA_TX_FRAME_SIZE) return false;
    if (s && xTaskGetSchedulerState()==taskSCHEDULER_RUNNING) {
        const TickType_t start=xTaskGetTickCount();
        for (;;) {
            taskENTER_CRITICAL();
            bool claimed=!s->meter_pending;
            if (claimed) s->meter_pending=1;
            taskEXIT_CRITICAL();
            if (claimed) break;
            if (xTaskGetTickCount()-start>=pdMS_TO_TICKS(150)) {
                s->stats.meter_tx_timeouts++;return false;
            }
            vTaskDelay(1);
        }
        s->meter_done=0;s->meter_error=0;
        s->meter_transaction=new_transaction();
        bool ok=send_frame_internal(OSLP_KIND_REQUEST,OSLP_SERVICE_METER_BRIDGE,
                                   OSLP_METER_TX,s->meter_transaction,frame,
                                   (uint16_t)length,true);
        TickType_t sent=xTaskGetTickCount();
        while(ok && !s->meter_done &&
              xTaskGetTickCount()-sent<pdMS_TO_TICKS(150)) vTaskDelay(1);
        ok=ok && s->meter_done && !s->meter_error;
        if(ok)s->stats.meter_tx_frames++;else s->stats.meter_tx_timeouts++;
        s->meter_pending=0;
        return ok;
    }
    /* Early boot has no link state/task yet. Use the SAME wire envelope and
     * wait for the matching TX_DONE while polling. No large permanent boot
     * buffer is needed. DWT is an independent 240-MHz counter; SysTick/RTOS
     * delays cannot be used before the scheduler. Never retry: TX_DONE loss
     * leaves command delivery uncertain. Boot replies are diagnostic only. */
    uint8_t raw[64],wire[66];
    if(++boot_meter_transaction==0)++boot_meter_transaction;
    oslp_frame_t f={.kind=OSLP_KIND_REQUEST,.flags=OSLP_FLAG_RAW,
        .service=OSLP_SERVICE_METER_BRIDGE,.opcode=OSLP_METER_TX,
        .transaction_id=boot_meter_transaction,
        .payload_length=(uint16_t)length,.payload=frame};
    size_t n=oslp_frame_build(&f,raw,sizeof(raw));
    n=oslp_cobs_encode(raw,n,wire,sizeof(wire)-1u);
    if(!n)return false;
    wire[n++]=0;
    CoreDebug->DEMCR|=CoreDebug_DEMCR_TRCENA_Msk;
    DWT->CTRL|=DWT_CTRL_CYCCNTENA_Msk;
    uint32_t saved=USART2->ctrl1;
    USART2->ctrl1=(saved | (1u<<13) | (1u<<3) | (1u<<2)) &
                  ~((1u<<5)|(1u<<7));
    uint32_t start=DWT->CYCCNT,limit=system_core_clock/5u;
    bool ok=false;
    for(size_t i=0;i<n;i++) {
        while(!(USART2->sts & USART_TDBE_FLAG))
            if((uint32_t)(DWT->CYCCNT-start)>limit)goto boot_done;
        USART2->dt=wire[i];
    }
    size_t used=0;bool discard=false;
    uint32_t last_feed=DWT->CYCCNT;
    while((uint32_t)(DWT->CYCCNT-start)<limit) {
        /* Stock-IAP handoff can inherit a running watchdog. An absent ESP
         * must produce a bounded transport timeout, not a watchdog reset. */
        if((uint32_t)(DWT->CYCCNT-last_feed)>=system_core_clock/1000u) {
            wdt_counter_reload();last_feed=DWT->CYCCNT;
        }
        if(!(USART2->sts & USART_RDBF_FLAG))continue;
        uint8_t b=(uint8_t)USART2->dt;
        if(b) {
            if(!discard && used<sizeof(wire))wire[used++]=b;
            else discard=true;
        } else {
            size_t count=discard?0:oslp_cobs_decode(wire,used,raw,sizeof(raw));
            oslp_frame_t reply;
            if(count && oslp_frame_parse(raw,count,&reply) &&
               reply.service==OSLP_SERVICE_METER_BRIDGE &&
               reply.opcode==OSLP_METER_TX_DONE &&
               reply.transaction_id==boot_meter_transaction) {
                ok=reply.kind==OSLP_KIND_ACK;goto boot_done;
            }
            used=0;discard=false;
        }
    }
boot_done:
    USART2->ctrl1=saved;
    if(ok)failed_stats.meter_tx_frames++;else failed_stats.meter_tx_timeouts++;
    return ok;
#else
    (void)frame;(void)length;return false;
#endif
}

bool esp_link_init(void)
{
    s=pvPortMalloc(sizeof(*s));
    if(!s){failed_stats.heap_failures++;return false;}
    memset(s,0,sizeof(*s));
    s->stats.meter_tx_frames=failed_stats.meter_tx_frames;
    s->stats.meter_tx_timeouts=failed_stats.meter_tx_timeouts;
#ifdef ESP_LINK_BRIDGE
    s->next_transaction=boot_meter_transaction;
#endif
    s->tx_mutex=xSemaphoreCreateMutex();
    if(!s->tx_mutex){failed_stats.heap_failures++;vPortFree(s);s=NULL;return false;}
    s->local_nonce=0x403A0000u ^ (uint32_t)(uintptr_t)s ^ system_core_clock;
#ifdef ESP_LINK_BRIDGE
    /* fpga_init owns the existing PA2/PA3 route. From here USART2 is the
     * always-on coprocessor transport, including while scope mode is active. */
    usart_init(USART2,ESP_LINK_BAUD,USART_DATA_8BITS,USART_STOP_1_BIT);
    usart_parity_selection_config(USART2,USART_PARITY_NONE);
    usart_transmitter_enable(USART2,TRUE);usart_receiver_enable(USART2,TRUE);
    usart_interrupt_enable(USART2,USART_RDBF_INT,TRUE);
    nvic_irq_enable(USART2_IRQn,5,0);usart_enable(USART2,TRUE);
#else
    crm_periph_clock_enable(CRM_GPIOE_PERIPH_CLOCK,TRUE);
    crm_periph_clock_enable(CRM_IOMUX_PERIPH_CLOCK,TRUE);
    crm_periph_clock_enable(CRM_UART8_PERIPH_CLOCK,TRUE);
    /* usart_init() only writes baud/data/stop fields; it is not a peripheral
     * deinitializer.  A guest image can inherit UART8 control state from the
     * stock IAP path, so force the documented CRM reset before configuration. */
    usart_reset(UART8);
    gpio_pin_remap_config(UART8_GMUX,FALSE);
    gpio_init_type g;gpio_default_para_init(&g);gpio_bits_set(GPIOE,GPIO_PINS_1);g.gpio_pins=GPIO_PINS_1;g.gpio_mode=ESP_LINK_SOFT_TX?GPIO_MODE_OUTPUT:GPIO_MODE_MUX;g.gpio_out_type=GPIO_OUTPUT_PUSH_PULL;g.gpio_drive_strength=GPIO_DRIVE_STRENGTH_STRONGER;gpio_init(GPIOE,&g);
    gpio_default_para_init(&g);g.gpio_pins=GPIO_PINS_0;g.gpio_mode=GPIO_MODE_INPUT;g.gpio_pull=GPIO_PULL_UP;gpio_init(GPIOE,&g);
    usart_init(UART8,ESP_LINK_BAUD,USART_DATA_8BITS,USART_STOP_1_BIT);
    usart_parity_selection_config(UART8,USART_PARITY_NONE);usart_transmitter_enable(UART8,ESP_LINK_SOFT_TX?FALSE:TRUE);usart_receiver_enable(UART8,TRUE);
    usart_interrupt_enable(UART8,USART_RDBF_INT,TRUE);nvic_irq_enable(UART8_IRQn,5,0);usart_enable(UART8,TRUE);
    /* From this point XMC is off whenever no LCD API owns it. UART8 remains
     * clocked during idle gaps, and every LCD transaction reverses the gates
     * atomically. This is required because PE0/PE1 double as XMC LB/UB. */
    lcd_uart_bus_arbitration_enable();
#endif
    return true;
}

bool esp_link_create_task(void)
{
    if(s && xTaskCreate(link_task,"esp-link",LINK_TASK_STACK,NULL,2,NULL)!=pdPASS){
        /* Fail closed: UART remains electrically configured, but no session is
         * advertised and no producer can block waiting for a missing task. */
        nvic_irq_disable(LINK_IRQ);usart_enable(LINK_UART,FALSE);s->available=false;
        lcd_uart_bus_arbitration_disable();
        failed_stats.task_create_failures++;
        vSemaphoreDelete(s->tx_mutex);vPortFree(s);s=NULL;
        return false;
    }
    return s != NULL;
}
size_t esp_link_heap_required(void)
{
    /* heap_4 prepends an aligned BlockLink_t (8 bytes on Cortex-M4) to
     * every allocation. StaticTask/Queue mirror the private dynamic object
     * sizes by FreeRTOS contract, so this is a conservative exact preflight. */
#define HEAP4_BLOCK_BYTES(n) (((size_t)(n) + 15u) & ~(size_t)7u)
    return HEAP4_BLOCK_BYTES(sizeof(link_state_t)) +
           HEAP4_BLOCK_BYTES(sizeof(StaticSemaphore_t)) +
           HEAP4_BLOCK_BYTES(LINK_TASK_STACK * sizeof(StackType_t)) +
           HEAP4_BLOCK_BYTES(sizeof(StaticTask_t));
#undef HEAP4_BLOCK_BYTES
}
void esp_link_note_preflight_failure(void){failed_stats.heap_failures++;}
bool esp_link_started(void){return s != NULL;}
bool esp_link_available(void){return s && s->available;}
const esp_link_stats_t *esp_link_stats(void){return s?&s->stats:&failed_stats;}
void esp_link_request_app_list(void){if(s&&s->available)(void)send_frame(OSLP_KIND_REQUEST,OSLP_SERVICE_APP,OSLP_APP_LIST,new_transaction(),(uint8_t[]){OSLP_RAW_ABI_VERSION},1);}
void esp_link_request_app_start(uint32_t id){uint8_t p[3]={OSLP_RAW_ABI_VERSION};put16(p+1,(uint16_t)id);if(s&&s->available)(void)send_frame(OSLP_KIND_REQUEST,OSLP_SERVICE_APP,OSLP_APP_START,new_transaction(),p,sizeof(p));}
void esp_link_request_app_stop(uint8_t reason){(void)reason;uint8_t p[1]={OSLP_RAW_ABI_VERSION};if(s&&s->available)(void)send_frame(OSLP_KIND_REQUEST,OSLP_SERVICE_APP,OSLP_APP_STOP,new_transaction(),p,sizeof(p));esp_activity_stop_local();}
void esp_link_send_key(uint8_t key,uint8_t action,uint16_t held){(void)held;uint8_t p[7]={OSLP_RAW_ABI_VERSION};put32(p+1,esp_activity_token());p[5]=(key>0)?(uint8_t)(key-1u):0;p[6]=action;if(s&&s->available&&esp_activity_active())(void)send_frame(OSLP_KIND_EVENT,OSLP_SERVICE_APP,OSLP_APP_INPUT,0,p,sizeof(p));}
void esp_link_ui_ack(uint16_t transaction){if(s&&s->available&&transaction)(void)send_frame(OSLP_KIND_ACK,OSLP_SERVICE_UI,OSLP_UI_ACTIVITY_STATE,transaction,NULL,0);}
