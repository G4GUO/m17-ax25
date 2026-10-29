/*
 * Tests for the KISS-to-burst bridge: queueing, duplicate suppression, the
 * access state machine, voice priority and the guard time.
 *
 * Time, DCD, PTT and randomness are all injected, so every case is
 * deterministic.
 *
 * BSD Zero Clause License.  See LICENSE.
 */
#include <stdio.h>
#include <string.h>

#include "m17_bridge.h"

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

static const uint8_t FRAME_A[] = { 0x82, 0x9A, 0x60, 0xA8, 0xA4, 0x40, 0xE0, 0x9A };
static const uint8_t FRAME_B[] = { 0x82, 0x9A, 0x60, 0xA8, 0xA4, 0x40, 0xE0, 0x9B };

/* Configuration with no randomness and no guard, so a tick can transmit
 * immediately: backoff and guard are exercised separately. */
static void cfg_plain(m17_bridge_config *cfg)
{
    m17_bridge_config_default(cfg);
    cfg->queue_depth = 4;
    cfg->max_slots   = 0;
    cfg->guard_ms    = 0;
    cfg->quiet_ms    = 0;
}

/* --------------------------------------------------------------- queueing */

static void test_push_and_duplicates(void)
{
    m17_bridge_config cfg;
    m17_bridge        b;

    cfg_plain(&cfg);
    m17_bridge_init(&b, &cfg);

    CHECK(m17_bridge_push(&b, FRAME_A, sizeof FRAME_A) == 1);
    CHECK(b.count == 1);

    /* same bytes again: a retransmit, not a second burst */
    CHECK(m17_bridge_push(&b, FRAME_A, sizeof FRAME_A) == 0);
    CHECK(b.count == 1);
    CHECK(b.dropped_dup == 1);

    CHECK(m17_bridge_push(&b, FRAME_B, sizeof FRAME_B) == 1);
    CHECK(b.count == 2);

    /* same length, different content, is not a duplicate */
    CHECK(m17_bridge_push(&b, FRAME_A, sizeof FRAME_A - 1) == 1);
    CHECK(b.count == 3);

    /* empty and oversized frames are refused */
    CHECK(m17_bridge_push(&b, FRAME_A, 0) == 0);
    CHECK(m17_bridge_push(&b, FRAME_A, M17_MAX_APP_BYTES + 1) == 0);
}

static void test_queue_full_drops_newest(void)
{
    m17_bridge_config cfg;
    m17_bridge        b;
    uint8_t           f[4] = { 1, 2, 3, 4 };

    cfg_plain(&cfg);
    cfg.queue_depth = 2;
    m17_bridge_init(&b, &cfg);

    CHECK(m17_bridge_push(&b, f, 4) == 1);
    f[0] = 9;
    CHECK(m17_bridge_push(&b, f, 4) == 1);
    f[0] = 7;
    CHECK(m17_bridge_push(&b, f, 4) == 0); /* dropped */
    CHECK(b.count == 2);
    CHECK(b.dropped_full == 1);

    /* the two oldest survive, in order */
    {
        size_t         len = 0;
        const uint8_t *head = m17_bridge_head(&b, &len);
        CHECK(len == 4);
        CHECK(head != NULL && head[0] == 1);
    }

    /* an identical frame is still recognised while full */
    f[0] = 1;
    CHECK(m17_bridge_push(&b, f, 4) == 0);
    CHECK(b.dropped_dup == 1);
}

static void test_queue_immutable_after_push(void)
{
    m17_bridge_config cfg;
    m17_bridge        b;
    uint8_t           f[3] = { 'a', 'b', 'c' };

    cfg_plain(&cfg);
    m17_bridge_init(&b, &cfg);
    CHECK(m17_bridge_push(&b, f, 3) == 1);
    f[0] = 'z'; /* caller reuses its buffer */

    {
        size_t         len;
        const uint8_t *head = m17_bridge_head(&b, &len);
        CHECK(len == 3 && head[0] == 'a' && head[2] == 'c');
    }
}

/* ----------------------------------------------------------- access cycle */

static void test_transmit_when_clear(void)
{
    m17_bridge_config cfg;
    m17_bridge        b;
    size_t            len = 0;
    const uint8_t    *head;

    cfg_plain(&cfg);
    m17_bridge_init(&b, &cfg);

    /* nothing queued: never transmit */
    CHECK(m17_bridge_tick(&b, 1000, 0, 0, 0) == 0);
    CHECK(m17_bridge_head(&b, &len) == NULL);

    CHECK(m17_bridge_push(&b, FRAME_A, sizeof FRAME_A) == 1);

    /* channel busy: wait */
    CHECK(m17_bridge_tick(&b, 1100, 1, 0, 0) == 0);
    /* clear: go */
    CHECK(m17_bridge_tick(&b, 1140, 0, 0, 0) == 1);

    head = m17_bridge_head(&b, &len);
    CHECK(head != NULL && len == sizeof FRAME_A);
    CHECK(head[0] == FRAME_A[0]);

    /* while the burst is in flight nothing else may start, even if the
     * channel looks clear and more frames are waiting */
    CHECK(m17_bridge_push(&b, FRAME_B, sizeof FRAME_B) == 1);
    CHECK(m17_bridge_tick(&b, 1200, 0, 0, 0) == 0);

    /* burst finished: the frame leaves the queue and the guard starts */
    m17_bridge_sent(&b, 1500);
    CHECK(b.sent == 1);
    CHECK(b.count == 1);
    CHECK(b.state == M17_BRIDGE_GUARD);

    /* this configuration has a zero guard, so the next frame goes at once */
    CHECK(m17_bridge_tick(&b, 1500, 0, 0, 0) == 1);
    {
        const uint8_t *next = m17_bridge_head(&b, &len);
        CHECK(next != NULL && next[0] == FRAME_B[0]);
    }
}

static void test_guard_blocks_own_tail(void)
{
    m17_bridge_config cfg;
    m17_bridge        b;

    cfg_plain(&cfg);
    cfg.guard_ms = 20;
    m17_bridge_init(&b, &cfg);

    CHECK(m17_bridge_push(&b, FRAME_A, sizeof FRAME_A) == 1);
    CHECK(m17_bridge_tick(&b, 0, 0, 0, 0) == 1);
    m17_bridge_sent(&b, 0);

    /* a second frame is waiting, but the key-down tail keeps us quiet */
    CHECK(m17_bridge_push(&b, FRAME_B, sizeof FRAME_B) == 1);
    CHECK(m17_bridge_tick(&b, 10, 0, 0, 0) == 0);
    CHECK(m17_bridge_tick(&b, 19, 0, 0, 0) == 0);
    CHECK(m17_bridge_tick(&b, 20, 0, 0, 0) == 1);
}

static void test_backoff_delay(void)
{
    m17_bridge_config cfg;
    m17_bridge        b;

    cfg_plain(&cfg);
    cfg.max_slots = 3;   /* backoff 0..3 slots */
    cfg.slot_ms   = 40;
    m17_bridge_init(&b, &cfg);

    CHECK(m17_bridge_push(&b, FRAME_A, sizeof FRAME_A) == 1);

    /* random word picks 2 slots = 80 ms */
    CHECK(m17_bridge_tick(&b, 1000, 0, 0, 2) == 0);
    CHECK(b.state == M17_BRIDGE_BACKOFF);
    CHECK(m17_bridge_tick(&b, 1039, 0, 0, 2) == 0);
    CHECK(m17_bridge_tick(&b, 1080, 0, 0, 2) == 1);

    /* zero slots means transmit on this tick */
    m17_bridge_init(&b, &cfg);
    CHECK(m17_bridge_push(&b, FRAME_A, sizeof FRAME_A) == 1);
    CHECK(m17_bridge_tick(&b, 2000, 0, 0, 0) == 1);
}

static void test_backoff_restarts_when_channel_goes_busy(void)
{
    m17_bridge_config cfg;
    m17_bridge        b;

    cfg_plain(&cfg);
    cfg.max_slots = 3;
    cfg.slot_ms   = 40;
    m17_bridge_init(&b, &cfg);

    CHECK(m17_bridge_push(&b, FRAME_A, sizeof FRAME_A) == 1);
    CHECK(m17_bridge_tick(&b, 1000, 0, 0, 3) == 0);   /* 3 slots = 120 ms */
    CHECK(b.state == M17_BRIDGE_BACKOFF);

    /* someone else keys up mid-backoff */
    CHECK(m17_bridge_tick(&b, 1050, 1, 0, 3) == 0);
    CHECK(b.state == M17_BRIDGE_IDLE);

    /* original deadline passes but the channel is still busy */
    CHECK(m17_bridge_tick(&b, 1200, 1, 0, 3) == 0);

    /* clear again: a fresh draw, this time 1 slot */
    CHECK(m17_bridge_tick(&b, 1300, 0, 0, 1) == 0);
    CHECK(b.state == M17_BRIDGE_BACKOFF);
    CHECK(m17_bridge_tick(&b, 1340, 0, 0, 1) == 1);
}

/* -------------------------------------------------------- voice priority */

static void test_voice_blocks_data(void)
{
    m17_bridge_config cfg;
    m17_bridge        b;

    cfg_plain(&cfg);
    m17_bridge_init(&b, &cfg);

    CHECK(m17_bridge_push(&b, FRAME_A, sizeof FRAME_A) == 1);

    /* operator keyed the mic */
    CHECK(m17_bridge_tick(&b, 500, 0, 1, 0) == 0);
    CHECK(m17_bridge_tick(&b, 900, 0, 1, 0) == 0);

    /* mic released: data may go */
    CHECK(m17_bridge_tick(&b, 1000, 0, 0, 0) == 1);

    /* a burst in flight ignores the mic */
    CHECK(m17_bridge_tick(&b, 1100, 0, 1, 0) == 0);
    CHECK(b.state == M17_BRIDGE_TX);
    m17_bridge_sent(&b, 1600);
}

static void test_quiet_period_after_voice(void)
{
    m17_bridge_config cfg;
    m17_bridge        b;

    cfg_plain(&cfg);
    cfg.quiet_ms = 3000;
    m17_bridge_init(&b, &cfg);

    CHECK(m17_bridge_push(&b, FRAME_A, sizeof FRAME_A) == 1);

    /* a voice over ends at t=1000 */
    CHECK(m17_bridge_tick(&b, 1000, 0, 1, 0) == 0);

    /* the pause between overs is not an invitation */
    CHECK(m17_bridge_tick(&b, 1500, 0, 0, 0) == 0);
    CHECK(m17_bridge_tick(&b, 3999, 0, 0, 0) == 0);
    CHECK(m17_bridge_tick(&b, 4000, 0, 0, 0) == 1);

    /* activity heard on the channel restarts the quiet period */
    m17_bridge_init(&b, &cfg);
    CHECK(m17_bridge_push(&b, FRAME_A, sizeof FRAME_A) == 1);
    CHECK(m17_bridge_tick(&b, 0, 1, 0, 0) == 0);     /* heard something */
    CHECK(m17_bridge_tick(&b, 2000, 0, 0, 0) == 0);  /* still too soon */
    CHECK(m17_bridge_tick(&b, 3000, 0, 0, 0) == 1);
}

static void test_time_wraparound(void)
{
    m17_bridge_config cfg;
    m17_bridge        b;

    cfg_plain(&cfg);
    cfg.guard_ms = 20;
    m17_bridge_init(&b, &cfg);

    CHECK(m17_bridge_push(&b, FRAME_A, sizeof FRAME_A) == 1);
    CHECK(m17_bridge_tick(&b, 0xFFFFFFF0u, 0, 0, 0) == 1);
    m17_bridge_sent(&b, 0xFFFFFFF0u);

    CHECK(m17_bridge_push(&b, FRAME_B, sizeof FRAME_B) == 1);
    CHECK(m17_bridge_tick(&b, 0xFFFFFFF8u, 0, 0, 0) == 0);  /* inside guard */
    CHECK(m17_bridge_tick(&b, 0x00000004u, 0, 0, 0) == 1);  /* wrapped past it */
}

int main(void)
{
    test_push_and_duplicates();
    test_queue_full_drops_newest();
    test_queue_immutable_after_push();
    test_transmit_when_clear();
    test_guard_blocks_own_tail();
    test_backoff_delay();
    test_backoff_restarts_when_channel_goes_busy();
    test_voice_blocks_data();
    test_quiet_period_after_voice();
    test_time_wraparound();

    printf("%d checks, %d failure%s\n", checks, failures, failures == 1 ? "" : "s");
    return failures == 0 ? 0 : 1;
}
