#include <stdio.h>
#include <string.h>
#include "esp_link_protocol.h"

static unsigned checks, failures;
#define CHECK(x) do { ++checks; if (!(x)) { ++failures; printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #x); } } while (0)

static void test_crc_vector(void)
{
    static const uint8_t v[] = "123456789";
    CHECK(oslp_crc32c(v, 9) == 0xE3069283u);
}

static void test_cobs_vectors(void)
{
    const uint8_t raw[] = { 0, 1, 2, 0, 3, 0 };
    uint8_t encoded[32], decoded[32];
    size_t en = oslp_cobs_encode(raw, sizeof(raw), encoded, sizeof(encoded));
    size_t de = oslp_cobs_decode(encoded, en, decoded, sizeof(decoded));
    CHECK(en != 0);
    CHECK(de == sizeof(raw));
    CHECK(memcmp(raw, decoded, sizeof(raw)) == 0);
    /* The MCU receiver deliberately decodes into its encoded buffer. COBS'
     * read cursor must remain ahead of the write cursor for this to work. */
    memcpy(decoded, encoded, en);
    de = oslp_cobs_decode(decoded, en, decoded, sizeof(decoded));
    CHECK(de == sizeof(raw));
    CHECK(memcmp(raw, decoded, sizeof(raw)) == 0);
    CHECK(oslp_cobs_decode((uint8_t[]){ 3, 1 }, 2, decoded, sizeof(decoded)) == 0);
}

static void test_frame_roundtrip_and_rejection(void)
{
    const uint8_t payload[] = { OSLP_RAW_ABI_VERSION, 0, 7, 0xAA };
    oslp_frame_t in = {
        .kind = OSLP_KIND_REQUEST, .flags = OSLP_FLAG_RAW,
        .service = OSLP_SERVICE_DEVICE, .opcode = OSLP_DEVICE_SUBSCRIBE,
        .session_id = 0x1234, .transaction_id = 9, .sequence = 42,
        .payload_length = sizeof(payload), .payload = payload,
    }, out;
    uint8_t decoded[OSLP_MAX_DECODED_FRAME];
    size_t n = oslp_frame_build(&in, decoded, sizeof(decoded));
    CHECK(n == OSLP_HEADER_SIZE + sizeof(payload) + OSLP_CRC_SIZE);
    CHECK(oslp_frame_parse(decoded, n, &out));
    CHECK(out.session_id == in.session_id && out.sequence == in.sequence);
    CHECK(out.payload_length == sizeof(payload));
    CHECK(memcmp(out.payload, payload, sizeof(payload)) == 0);

    decoded[7] ^= 1;
    CHECK(!oslp_frame_parse(decoded, n, &out)); /* header is CRC-covered */
    decoded[7] ^= 1;
    CHECK(!oslp_frame_parse(decoded, n - 1, &out)); /* truncation */
    decoded[14] = 0xFF;
    decoded[15] = 0xFF;
    CHECK(!oslp_frame_parse(decoded, n, &out)); /* hostile length */
}

int main(void)
{
    test_crc_vector();
    test_cobs_vectors();
    test_frame_roundtrip_and_rejection();
    printf("OSLP protocol: %u checks, %u failures\n", checks, failures);
    return failures ? 1 : 0;
}
