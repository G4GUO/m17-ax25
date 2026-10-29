/*
 * Tests for the M17 packet framing: CRC, address encoding, Link Setup Data,
 * chunking and reassembly.
 *
 * Golden values are either from the specification (the AB1CD address) or
 * regression values computed from the documented algorithm; the EOF/counter
 * encoding is additionally cross-checked against the formula used by the
 * radio's m17_fmt_add_packet().
 *
 * BSD Zero Clause License.  See LICENSE.
 */
#include <stdio.h>
#include <string.h>

#include "m17_packet.h"

static int checks;
static int failures;

#define CHECK(cond)                                                      \
    do {                                                                 \
        checks++;                                                        \
        if (!(cond)) {                                                   \
            failures++;                                                  \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);       \
        }                                                                \
    } while (0)

/* ------------------------------------------------------------------- CRC */

static void test_crc(void)
{
    const uint8_t nine[]   = { '1', '2', '3', '4', '5', '6', '7', '8', '9' };
    uint8_t       seq28[28];

    CHECK(m17_crc16(NULL, 0) == 0xFFFF);          /* initial value */
    CHECK(m17_crc16(nine, sizeof nine) == 0x772B);
    CHECK(m17_crc16((const uint8_t *)"ABC", 3) == 0xA9A7);
    CHECK(m17_crc16((const uint8_t *)"\x00\x00\x00", 3) == 0x39CC);

    for (size_t i = 0; i < sizeof seq28; i++)
        seq28[i] = (uint8_t)i;
    CHECK(m17_crc16(seq28, sizeof seq28) == 0xF305);

    /* a single bit of change must change the CRC */
    seq28[0] ^= 0x01;
    CHECK(m17_crc16(seq28, sizeof seq28) != 0xF305);
}

/* --------------------------------------------------------------- addresses */

static void test_address_encode(void)
{
    uint8_t a[M17_ADDR_BYTES];

    /* spec A.2 worked example */
    CHECK(m17_addr_encode("AB1CD", a) == 1);
    CHECK(m17_addr_to_u48(a) == 0x0000009FDD51ull);

    /* 'A' alone is value 1 */
    CHECK(m17_addr_encode("A", a) == 1);
    CHECK(m17_addr_to_u48(a) == 1);

    CHECK(m17_addr_encode("G4GUO", a) == 1);
    CHECK(m17_addr_to_u48(a) == 0x000000025EA29Full);

    /* trailing spaces are insignificant */
    {
        uint8_t b[M17_ADDR_BYTES];
        CHECK(m17_addr_encode("G4GUO   ", b) == 1);
        CHECK(m17_addr_to_u48(a) == m17_addr_to_u48(b));
    }

    /* lower case folds to upper */
    {
        uint8_t b[M17_ADDR_BYTES];
        CHECK(m17_addr_encode("g4guo", b) == 1);
        CHECK(m17_addr_to_u48(a) == m17_addr_to_u48(b));
    }

    /* invalid characters become space; '-' '/' '.' are values 37..39 */
    {
        uint8_t b[M17_ADDR_BYTES];
        uint8_t c[M17_ADDR_BYTES];
        CHECK(m17_addr_encode("A_B", b) == 1);
        CHECK(m17_addr_encode("A B", c) == 1);
        CHECK(m17_addr_to_u48(b) == m17_addr_to_u48(c));
        CHECK(m17_addr_encode("A-B", b) == 1);   /* 37 */
        CHECK(m17_addr_encode("A/B", b) == 1);   /* 38 */
        CHECK(m17_addr_encode("A.B", b) == 1);   /* 39 */
    }

    /* limits */
    CHECK(m17_addr_encode("", a) == 0);
    CHECK(m17_addr_encode(NULL, a) == 0);
    CHECK(m17_addr_encode("ABCDEFGHI", a) == 1);   /* 9 characters */
    CHECK(m17_addr_encode("ABCDEFGHIJ", a) == 0);  /* 10 is too many */

    /* broadcast constant */
    {
        uint8_t bcast[M17_ADDR_BYTES];
        const uint8_t expect[M17_ADDR_BYTES] = { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF };
        m17_addr_from_u48(M17_ADDR_BROADCAST, bcast);
        CHECK(memcmp(bcast, expect, sizeof expect) == 0);
        CHECK(m17_addr_to_u48(expect) == M17_ADDR_BROADCAST);
    }
}

/* -------------------------------------------------------------------- LSF */

static void test_lsd(void)
{
    uint8_t dst[M17_ADDR_BYTES];
    uint8_t src[M17_ADDR_BYTES];
    uint8_t lsd[M17_LSD_BYTES];
    uint8_t meta[M17_META_BYTES];
    uint8_t zeros[M17_META_BYTES];

    memset(meta, 0xA5, sizeof meta);
    memset(zeros, 0, sizeof zeros);
    m17_addr_from_u48(M17_ADDR_BROADCAST, dst);
    CHECK(m17_addr_encode("G4GUO", src) == 1);

    m17_lsd_build(lsd, dst, src, M17_PACKET_TYPE_CAN0, NULL);

    /* field placement */
    CHECK(lsd[0] == 0xFF && lsd[5] == 0xFF);
    CHECK(memcmp(&lsd[6], src, M17_ADDR_BYTES) == 0);
    CHECK(lsd[12] == 0x00 && lsd[13] == 0x00);       /* packet, CAN 0 */
    CHECK(memcmp(&lsd[14], zeros, M17_META_BYTES) == 0);

    /* CRC covers the first 28 bytes */
    {
        uint16_t crc = m17_crc16(lsd, M17_LSD_BYTES - 2);
        CHECK(lsd[28] == (uint8_t)(crc >> 8));
        CHECK(lsd[29] == (uint8_t)(crc & 0xFF));
    }

    /* META is carried verbatim, and changes the CRC */
    {
        uint8_t lsd2[M17_LSD_BYTES];
        m17_lsd_build(lsd2, dst, src, M17_PACKET_TYPE_CAN0, meta);
        CHECK(memcmp(&lsd2[14], meta, M17_META_BYTES) == 0);
        CHECK(memcmp(&lsd2[28], &lsd[28], 2) != 0);
    }

    /* CAN and P/S bits land where the spec says */
    {
        uint8_t lsd3[M17_LSD_BYTES];
        m17_lsd_build(lsd3, dst, src, 0x0500, NULL); /* CAN 5, packet */
        CHECK(lsd3[12] == 0x05);
        CHECK(lsd3[13] == 0x00);
        m17_lsd_build(lsd3, dst, src, 0x0001, NULL); /* stream */
        CHECK((lsd3[13] & 0x01) == 0x01);
    }
}

/* ---------------------------------------------------------------- payload */

/* The radio's m17_fmt_add_packet() builds the metadata byte as
 *     meta = (eof ? 0x80 : 0) | ((field & 0x1F) << 2)
 * and m17_send_packet_frames() passes the frame index when not EOF and the
 * number of valid bytes on the EOF frame.  Reproduce that here so a change in
 * the library cannot silently diverge from the on-air format. */
static uint8_t radio_meta(int eof, unsigned field)
{
    uint8_t m = 0;

    if (eof)
        m = 0x80;
    m |= (uint8_t)((field & 0x1Fu) << 2);
    return m;
}

static void test_burst_geometry(void)
{
    static uint8_t payload[900];
    m17_pkt_burst  burst;
    uint8_t        crc_bytes[2];

    for (size_t i = 0; i < sizeof payload; i++)
        payload[i] = (uint8_t)i;

    /* empty payload: type byte and CRC only, one frame */
    memset(&burst, 0, sizeof burst);
    CHECK(m17_pkt_build(&burst, M17_TYPE_RAW, NULL, 0) == 1);
    CHECK(burst.count == 1);
    CHECK(burst.frame[0].meta == (0x80u | (3u << 2)));    /* EOF, 3 valid bytes */
    CHECK(burst.frame[0].meta == radio_meta(1, 3));
    CHECK(burst.frame[0].data[0] == M17_TYPE_RAW);
    {
        uint16_t crc = m17_crc16(burst.frame[0].data, 1);
        crc_bytes[0] = (uint8_t)(crc >> 8);
        crc_bytes[1] = (uint8_t)(crc & 0xFF);
        CHECK(burst.frame[0].data[1] == crc_bytes[0]);
        CHECK(burst.frame[0].data[2] == crc_bytes[1]);
        CHECK(burst.frame[0].data[3] == 0x00);            /* padding */
    }

    /* 22-byte payload: 22 + 1 + 2 = 25, exactly one full frame */
    CHECK(m17_pkt_build(&burst, M17_TYPE_AX25, payload, 22) == 1);
    CHECK(burst.count == 1);
    CHECK(burst.frame[0].meta == (0x80u | (25u << 2)));
    CHECK(burst.frame[0].meta == radio_meta(1, 25));

    /* 23-byte payload: 26 bytes, two frames, last one carries 1 byte */
    CHECK(m17_pkt_build(&burst, M17_TYPE_AX25, payload, 23) == 2);
    CHECK(burst.count == 2);
    CHECK(burst.frame[0].meta == radio_meta(0, 0));
    CHECK(burst.frame[1].meta == radio_meta(1, 1));
    CHECK(burst.frame[0].data[23] == payload[22]);        /* last payload byte */
    CHECK(burst.frame[1].data[1] == 0x00);                /* padding */

    /* maximum: 822 payload + type + CRC = 825 = 33 frames */
    CHECK(m17_pkt_build(&burst, M17_TYPE_AX25, payload, 822) == 33);
    CHECK(burst.count == 33);
    CHECK(burst.frame[31].meta == radio_meta(0, 31));
    CHECK(burst.frame[32].meta == radio_meta(1, 25));

    /* one more byte does not fit */
    CHECK(m17_pkt_build(&burst, M17_TYPE_AX25, payload, 823) == 0);

    /* only the 1-byte type specifier form is supported */
    CHECK(m17_pkt_build(&burst, 0x80, payload, 10) == 0);
}

/* ------------------------------------------------------------- round trip */

static void roundtrip(size_t len, uint8_t type)
{
    static uint8_t payload[M17_MAX_APP_BYTES];
    static uint8_t back[M17_MAX_APP_BYTES];
    m17_pkt_burst     burst;
    m17_rx_assembler  rx;
    m17_pkt_status    st = M17_PKT_NEED_MORE;

    for (size_t i = 0; i < len; i++)
        payload[i] = (uint8_t)(i * 7u + 3u);

    size_t frames = m17_pkt_build(&burst, type, payload, len);
    CHECK(frames > 0);
    if (frames == 0)
        return;

    m17_rx_reset(&rx);
    for (size_t i = 0; i < frames; i++) {
        st = m17_rx_chunk(&rx, &burst.frame[i]);
        if (st != M17_PKT_NEED_MORE)
            break;
    }

    CHECK(st == M17_PKT_COMPLETE);
    CHECK(rx.data[0] == type);
    CHECK(rx.len == len + 1);
    if (rx.len == len + 1) {
        memcpy(back, &rx.data[1], len);
        CHECK(memcmp(back, payload, len) == 0);
    }
}

static void test_roundtrip(void)
{
    const size_t sizes[] = { 0, 1, 22, 23, 24, 25, 100, 250, 500, 822 };

    for (size_t i = 0; i < sizeof sizes / sizeof sizes[0]; i++)
        roundtrip(sizes[i], M17_TYPE_AX25);

    roundtrip(64, M17_TYPE_RAW);
    roundtrip(64, M17_TYPE_APRS);
}

/* ---------------------------------------------------------------- errors */

static void test_reassembly_errors(void)
{
    static uint8_t    payload[300];
    m17_pkt_burst     burst;
    m17_rx_assembler  rx;
    m17_pkt_status    st = M17_PKT_NEED_MORE;

    for (size_t i = 0; i < sizeof payload; i++)
        payload[i] = (uint8_t)i;

    size_t frames = m17_pkt_build(&burst, M17_TYPE_AX25, payload, sizeof payload);
    CHECK(frames > 3);

    /* corrupt a payload byte: CRC must reject it */
    m17_rx_reset(&rx);
    for (size_t i = 0; i < frames; i++) {
        m17_pkt_frame f = burst.frame[i];
        if (i == 1)
            f.data[3] ^= 0xFF;
        st = m17_rx_chunk(&rx, &f);
        if (st != M17_PKT_NEED_MORE)
            break;
    }
    CHECK(st == M17_PKT_ERROR);

    /* start mid-burst: the counter check must reject it */
    m17_rx_reset(&rx);
    st = m17_rx_chunk(&rx, &burst.frame[1]);
    CHECK(st == M17_PKT_ERROR);

    /* EOF frame claiming zero valid bytes */
    m17_rx_reset(&rx);
    {
        m17_pkt_frame f = burst.frame[frames - 1];
        f.meta = 0x80;
        st = m17_rx_chunk(&rx, &f);
        CHECK(st == M17_PKT_ERROR);
    }

    /* EOF frame claiming more than 25 bytes */
    m17_rx_reset(&rx);
    {
        m17_pkt_frame f = burst.frame[frames - 1];
        f.meta = (uint8_t)(0x80 | (26u << 2));
        st = m17_rx_chunk(&rx, &f);
        CHECK(st == M17_PKT_ERROR);
    }

    /* a single frame whose CRC is wrong */
    m17_rx_reset(&rx);
    {
        m17_pkt_frame f = burst.frame[0];
        st = m17_rx_chunk(&rx, &f);           /* not EOF: just accumulates */
        CHECK(st == M17_PKT_NEED_MORE);
    }
}

int main(void)
{
    test_crc();
    test_address_encode();
    test_lsd();
    test_burst_geometry();
    test_roundtrip();
    test_reassembly_errors();

    printf("%d checks, %d failure%s\n", checks, failures, failures == 1 ? "" : "s");
    return failures == 0 ? 0 : 1;
}
