#include "esp_link_protocol.h"

static uint16_t get_u16(const uint8_t *p)
{
    return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}

static void put_u16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
}

static uint32_t get_u32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static void put_u32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

uint32_t oslp_crc32c(const uint8_t *data, size_t length)
{
    uint32_t crc = 0xFFFFFFFFu;
    for (size_t i = 0; i < length; ++i) {
        crc ^= data[i];
        for (unsigned bit = 0; bit < 8; ++bit)
            crc = (crc >> 1) ^ (0x82F63B78u & (uint32_t)-(int32_t)(crc & 1u));
    }
    return ~crc;
}

size_t oslp_cobs_encode(const uint8_t *input, size_t length,
                        uint8_t *output, size_t capacity)
{
    if (!output || capacity == 0 || (!input && length != 0)) return 0;
    size_t read = 0, write = 1, code_pos = 0;
    uint8_t code = 1;

    while (read < length) {
        if (input[read] == 0) {
            if (code_pos >= capacity) return 0;
            output[code_pos] = code;
            code_pos = write++;
            if (write > capacity) return 0;
            code = 1;
            ++read;
        } else {
            if (write >= capacity) return 0;
            output[write++] = input[read++];
            if (++code == 0xFF) {
                output[code_pos] = code;
                code_pos = write++;
                if (write > capacity) return 0;
                code = 1;
            }
        }
    }
    if (code_pos >= capacity) return 0;
    output[code_pos] = code;
    return write;
}

size_t oslp_cobs_decode(const uint8_t *input, size_t length,
                        uint8_t *output, size_t capacity)
{
    if (!input || !output) return 0;
    size_t read = 0, write = 0;
    while (read < length) {
        uint8_t code = input[read++];
        if (code == 0) return 0;
        size_t copy = (size_t)code - 1u;
        if (copy > length - read || copy > capacity - write) return 0;
        for (size_t i = 0; i < copy; ++i) output[write++] = input[read++];
        if (code != 0xFF && read < length) {
            if (write >= capacity) return 0;
            output[write++] = 0;
        }
    }
    return write;
}

size_t oslp_frame_build(const oslp_frame_t *f,
                        uint8_t *out, size_t capacity)
{
    if (!f || !out || f->payload_length > OSLP_MAX_PAYLOAD ||
        (f->payload_length && !f->payload)) return 0;
    size_t total = OSLP_HEADER_SIZE + f->payload_length + OSLP_CRC_SIZE;
    if (capacity < total) return 0;

    out[0] = OSLP_MAGIC_0; out[1] = OSLP_MAGIC_1;
    out[2] = OSLP_VERSION_MAJOR; out[3] = OSLP_VERSION_MINOR;
    out[4] = f->kind; out[5] = f->flags;
    out[6] = f->service; out[7] = f->opcode;
    put_u16(out + 8, f->session_id);
    put_u16(out + 10, f->transaction_id);
    put_u16(out + 12, f->sequence);
    put_u16(out + 14, f->payload_length);
    for (uint16_t i = 0; i < f->payload_length; ++i)
        out[OSLP_HEADER_SIZE + i] = f->payload[i];
    put_u32(out + OSLP_HEADER_SIZE + f->payload_length,
            oslp_crc32c(out, OSLP_HEADER_SIZE + f->payload_length));
    return total;
}

bool oslp_frame_parse(const uint8_t *d, size_t length, oslp_frame_t *f)
{
    if (!d || !f || length < OSLP_HEADER_SIZE + OSLP_CRC_SIZE ||
        length > OSLP_MAX_DECODED_FRAME || d[0] != OSLP_MAGIC_0 ||
        d[1] != OSLP_MAGIC_1 || d[2] != OSLP_VERSION_MAJOR) return false;
    uint16_t payload_len = get_u16(d + 14);
    if ((size_t)payload_len + OSLP_HEADER_SIZE + OSLP_CRC_SIZE != length)
        return false;
    uint32_t expected = get_u32(d + OSLP_HEADER_SIZE + payload_len);
    if (expected != oslp_crc32c(d, OSLP_HEADER_SIZE + payload_len)) return false;
    if ((d[5] & (OSLP_FLAG_CBOR | OSLP_FLAG_RAW)) ==
        (OSLP_FLAG_CBOR | OSLP_FLAG_RAW)) return false;

    f->kind = d[4]; f->flags = d[5]; f->service = d[6]; f->opcode = d[7];
    f->session_id = get_u16(d + 8);
    f->transaction_id = get_u16(d + 10);
    f->sequence = get_u16(d + 12);
    f->payload_length = payload_len;
    f->payload = d + OSLP_HEADER_SIZE;
    return true;
}
