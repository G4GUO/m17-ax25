/*
 * Golden-vector tests for the KISS framing codec.
 *
 * No framework: each case is a function, CHECK reports the first failure with
 * a line number, and main() returns non-zero if anything failed.
 *
 * BSD Zero Clause License.  See LICENSE.
 */
#include <stdio.h>
#include <string.h>

#include "kiss.h"

static int checks;
static int failures;

#define CHECK(cond)                                                      \
    do {                                                                 \
        checks++;                                                        \
        if (!(cond)) {                                                   \
            failures++;                                                  \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);      \
        }                                                                \
    } while (0)

static void hexdump(const char *label, const uint8_t *p, size_t n)
{
    printf("  %-12s", label);
    for (size_t i = 0; i < n; i++)
        printf(" %02X", p[i]);
    printf("\n");
}

/* Collector for the push/callback interface. */
#define MAX_COLLECTED 8

typedef struct {
    size_t   count;
    uint8_t  port[MAX_COLLECTED];
    uint8_t  command[MAX_COLLECTED];
    size_t   len[MAX_COLLECTED];
    uint8_t  data[MAX_COLLECTED][64];
} collected;

static void collect(void *ctx, const kiss_frame *frame, const uint8_t *data)
{
    collected *c = ctx;

    if (c->count >= MAX_COLLECTED)
        return;

    c->port[c->count]    = frame->port;
    c->command[c->count] = frame->command;
    c->len[c->count]     = frame->len;
    if (frame->len > sizeof c->data[0])
        c->len[c->count] = 0;
    else if (frame->len > 0)
        memcpy(c->data[c->count], data, frame->len);
    c->count++;
}

/* 1. A plain frame: FEND, header 0x00, "ABC", FEND. */
static void test_plain_frame(void)
{
    uint8_t buf[64];
    kiss_decoder d;
    kiss_frame f;
    int got = 0;
    const uint8_t in[] = { 0xC0, 0x00, 'A', 'B', 'C', 0xC0 };

    kiss_decoder_init(&d, buf, sizeof buf);
    for (size_t i = 0; i < sizeof in; i++)
        got += kiss_decoder_byte(&d, in[i], &f);

    CHECK(got == 1);
    CHECK(f.port == 0);
    CHECK(f.command == 0);
    CHECK(f.len == 3);
    CHECK(f.overrun == 0);
    CHECK(memcmp(buf, "ABC", 3) == 0);
}

/* 2. Escapes: 0xC0 and 0xDB in the payload survive a round trip, and 0x7E is
 *    not touched (only FEND and FESC are escaped). */
static void test_escapes(void)
{
    uint8_t buf[64];
    kiss_decoder d;
    kiss_frame f;
    int got = 0;
    const uint8_t payload[] = { 0x7E, 0xC0, 0xDB, 0x00 };
    const uint8_t wire[] = {
        0xC0, 0x00, 0x7E, 0xDB, 0xDC, 0xDB, 0xDD, 0x00, 0xC0
    };

    kiss_decoder_init(&d, buf, sizeof buf);
    for (size_t i = 0; i < sizeof wire; i++)
        got += kiss_decoder_byte(&d, wire[i], &f);

    CHECK(got == 1);
    CHECK(f.len == sizeof payload);
    if (f.len == sizeof payload) {
        CHECK(memcmp(buf, payload, sizeof payload) == 0);
        if (memcmp(buf, payload, sizeof payload) != 0)
            hexdump("got", buf, f.len);
    }
}

/* 3. The encoder must produce exactly the vector from test 2. */
static void test_encode_golden(void)
{
    const uint8_t payload[] = { 0x7E, 0xC0, 0xDB, 0x00 };
    const uint8_t expect[] = {
        0xC0, 0x00, 0x7E, 0xDB, 0xDC, 0xDB, 0xDD, 0x00, 0xC0
    };
    uint8_t out[64];
    size_t n = kiss_encode(0, 0, payload, sizeof payload, out, sizeof out);

    CHECK(n == sizeof expect);
    if (n == sizeof expect) {
        CHECK(memcmp(out, expect, sizeof expect) == 0);
        if (memcmp(out, expect, sizeof expect) != 0)
            hexdump("got", out, n);
    }

    /* exact-size buffer is enough */
    CHECK(kiss_encode(0, 0, payload, sizeof payload, out, sizeof expect) == sizeof expect);
    /* one byte short must fail cleanly and write nothing */
    memset(out, 0xAA, sizeof out);
    CHECK(kiss_encode(0, 0, payload, sizeof payload, out, sizeof expect - 1) == 0);
    CHECK(out[0] == 0xAA);
}

/* 4. Port and command land in the right nibbles. */
static void test_port_command(void)
{
    uint8_t buf[64];
    kiss_decoder d;
    kiss_frame f;

    /* port 1 command 0 */
    kiss_decoder_init(&d, buf, sizeof buf);
    {
        const uint8_t in[] = { 0xC0, 0x10, 'X', 0xC0 };
        for (size_t i = 0; i < sizeof in; i++)
            (void)kiss_decoder_byte(&d, in[i], &f);
    }
    CHECK(f.port == 1);
    CHECK(f.command == 0);

    /* port 0 command 3 = TXDELAY parameter, one byte of data */
    kiss_decoder_init(&d, buf, sizeof buf);
    {
        const uint8_t in[] = { 0xC0, 0x03, 0x0A, 0xC0 };
        for (size_t i = 0; i < sizeof in; i++)
            (void)kiss_decoder_byte(&d, in[i], &f);
    }
    CHECK(f.port == 0);
    CHECK(f.command == 3);
    CHECK(f.len == 1);
    CHECK(buf[0] == 0x0A);
}

/* 5. Zero-length frame: header byte and nothing else. */
static void test_zero_length(void)
{
    uint8_t buf[16];
    kiss_decoder d;
    kiss_frame f;
    const uint8_t in[] = { 0xC0, 0x00, 0xC0 };
    uint8_t wire[16];

    kiss_decoder_init(&d, buf, sizeof buf);
    int got = 0;
    for (size_t i = 0; i < sizeof in; i++)
        got += kiss_decoder_byte(&d, in[i], &f);
    CHECK(got == 1);
    CHECK(f.len == 0);

    CHECK(kiss_encode(0, 0, NULL, 0, wire, sizeof wire) == 3);
    CHECK(wire[0] == 0xC0 && wire[1] == 0x00 && wire[2] == 0xC0);
}

/* 6. Back-to-back frames in one block, and the same bytes arriving one at a
 *    time, must both yield two frames with the right contents. */
static void test_stream_of_two(void)
{
    const uint8_t stream[] = {
        0xC0, 0x00, 'O', 'N', 'E', 0xC0,
        0xC0, 0x00, 'T', 'W', 'O', 0xC0
    };
    uint8_t buf[64];
    kiss_decoder d;
    collected c;

    memset(&c, 0, sizeof c);
    kiss_decoder_init(&d, buf, sizeof buf);
    CHECK(kiss_decoder_push(&d, stream, sizeof stream, collect, &c) == 2);
    CHECK(c.count == 2);
    CHECK(c.len[0] == 3 && memcmp(c.data[0], "ONE", 3) == 0);
    CHECK(c.len[1] == 3 && memcmp(c.data[1], "TWO", 3) == 0);

    memset(&c, 0, sizeof c);
    kiss_decoder_init(&d, buf, sizeof buf);
    for (size_t i = 0; i < sizeof stream; i++)
        (void)kiss_decoder_push(&d, &stream[i], 1, collect, &c);
    CHECK(c.count == 2);
    CHECK(c.len[1] == 3 && memcmp(c.data[1], "TWO", 3) == 0);
}

/* 7. Leading FENDs are noise, not empty frames. */
static void test_leading_fends(void)
{
    const uint8_t stream[] = { 0xC0, 0xC0, 0xC0, 0x00, 'X', 0xC0 };
    uint8_t buf[32];
    kiss_decoder d;
    collected c;

    memset(&c, 0, sizeof c);
    kiss_decoder_init(&d, buf, sizeof buf);
    CHECK(kiss_decoder_push(&d, stream, sizeof stream, collect, &c) == 1);
    CHECK(c.count == 1);
    CHECK(c.len[0] == 1 && c.data[0][0] == 'X');
}

/* 8. A frame longer than the buffer is dropped, and the decoder recovers for
 *    the next frame. */
static void test_overrun(void)
{
    uint8_t buf[4];
    kiss_decoder d;
    kiss_frame f;
    const uint8_t stream[] = {
        0xC0, 0x00, 1, 2, 3, 4, 5, 6, 0xC0,
        0xC0, 0x00, 'O', 'K', 0xC0
    };
    int got = 0;

    kiss_decoder_init(&d, buf, sizeof buf);
    for (size_t i = 0; i < sizeof stream; i++) {
        if (kiss_decoder_byte(&d, stream[i], &f)) {
            got++;
            if (got == 1) {
                CHECK(f.overrun == 1);
                CHECK(f.len == 0);
            } else {
                CHECK(f.overrun == 0);
                CHECK(f.len == 2);
                CHECK(memcmp(buf, "OK", 2) == 0);
            }
        }
    }
    CHECK(got == 2);
}

/* 9. Round trip of a maximum-size payload containing escape bytes. */
typedef struct {
    size_t  len;
    uint8_t data[KISS_MAX_PAYLOAD];
} big_frame;

static void collect_big(void *ctx, const kiss_frame *frame, const uint8_t *data)
{
    big_frame *b = ctx;

    b->len = frame->len;
    if (frame->len <= sizeof b->data)
        memcpy(b->data, data, frame->len);
}

static void test_max_payload_roundtrip(void)
{
    static uint8_t payload[KISS_MAX_PAYLOAD];
    static uint8_t wire[KISS_MAX_PAYLOAD * 2 + 8];
    static uint8_t back[KISS_MAX_PAYLOAD];
    kiss_decoder d;
    big_frame b;

    for (size_t i = 0; i < sizeof payload; i++) {
        /* exercise every third byte as an escape candidate */
        payload[i] = (i % 3 == 0) ? 0xC0 : (i % 3 == 1 ? 0xDB : (uint8_t)i);
    }

    size_t n = kiss_encode(0, 0, payload, sizeof payload, wire, sizeof wire);
    CHECK(n > sizeof payload);
    CHECK(n <= sizeof wire);

    memset(&b, 0, sizeof b);
    kiss_decoder_init(&d, back, sizeof back);
    CHECK(kiss_decoder_push(&d, wire, n, collect_big, &b) == 1);
    CHECK(b.len == sizeof payload);
    CHECK(d.overrun == 0);
    CHECK(memcmp(b.data, payload, sizeof payload) == 0);
}

/* 10. A malformed escape is recovered, not fatal. */
static void test_bad_escape(void)
{
    uint8_t buf[16];
    kiss_decoder d;
    kiss_frame f;
    /* 0xDB 0x00 -> 0x00 ^ 0x20 = 0x20 (space) */
    const uint8_t in[] = { 0xC0, 0x00, 0xDB, 0x00, 0xC0 };
    int got = 0;

    kiss_decoder_init(&d, buf, sizeof buf);
    for (size_t i = 0; i < sizeof in; i++)
        got += kiss_decoder_byte(&d, in[i], &f);

    CHECK(got == 1);
    CHECK(f.len == 1);
    CHECK(buf[0] == 0x20);
}

int main(void)
{
    test_plain_frame();
    test_escapes();
    test_encode_golden();
    test_port_command();
    test_zero_length();
    test_stream_of_two();
    test_leading_fends();
    test_overrun();
    test_max_payload_roundtrip();
    test_bad_escape();

    printf("%d checks, %d failure%s\n", checks, failures, failures == 1 ? "" : "s");
    return failures == 0 ? 0 : 1;
}
