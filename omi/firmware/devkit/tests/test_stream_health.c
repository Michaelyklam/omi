#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>

#include "stream_health.h"

static struct stream_health health;

static void start(uint32_t now)
{
    stream_health_init(&health, 4);
    stream_health_connection(&health, now, true, 0);
    stream_health_subscription(&health, now, true);
    stream_health_expected(&health, now, true);
}

static void progress(uint32_t now, unsigned stages)
{
    for (int i = 0; i < STREAM_STAGE_COUNT; i++) {
        if (stages & (1U << i)) {
            stream_health_progress(&health, now, (enum stream_stage) i);
        }
    }
}

int main(int argc, char **argv)
{
    if (argc == 2 && strcmp(argv[1], "--snapshot") == 0) {
        start(100);
        progress(4999, 7);
        stream_health_error(&health, 5000, STREAM_BLE, -ENOMEM);
        stream_health_poll(&health, 5100);
        stream_health_connection(&health, 6000, false, 8);
        uint8_t wire[STREAM_HEALTH_WIRE_SIZE];
        stream_health_encode(&health, 6001, wire);
        return fwrite(wire, 1, sizeof(wire), stdout) == sizeof(wire) ? 0 : 1;
    }

    /* Each stage is independently classified, using the earliest missing
     * handoff rather than reporting every downstream stage as the cause.
     */
    for (int stalled = 0; stalled < STREAM_STAGE_COUNT; stalled++) {
        start(100);
        progress(5099, (1U << stalled) - 1U);
        assert(!stream_health_poll(&health, 5099));
        assert(stream_health_poll(&health, 5100));
        assert(health.stalled_stage == stalled);
        uint32_t sequence = health.sequence;
        assert(!stream_health_poll(&health, 6000));
        assert(health.sequence == sequence);
        progress(6100, 15);
        assert(stream_health_poll(&health, 6100));
        assert(health.stalled_stage == -1);
        assert(health.events[(health.event_head + 15U) % 16U].type == STREAM_RESUMED);
    }

    /* BLE stalls first; queue backpressure must not change attribution later. */
    start(0);
    progress(100, 15);
    progress(500, 7);
    progress(5200, 3);
    assert(stream_health_poll(&health, 5200));
    assert(health.stalled_stage == STREAM_BLE);
    progress(6000, 3);
    assert(!stream_health_poll(&health, 6000));
    assert(health.stalled_stage == STREAM_BLE);

    /* Silence is still real microphone progress. Healthy frames never alert. */
    start(0);
    for (uint32_t now = 0; now < 60000; now += 100) {
        progress(now, 15);
        assert(!stream_health_poll(&health, now));
    }

    /* A connected client that has not subscribed is not a broken stream. */
    stream_health_init(&health, 0);
    stream_health_connection(&health, 0, true, 0);
    stream_health_expected(&health, 0, true);
    assert(!stream_health_poll(&health, 60000));
    stream_health_subscription(&health, 60000, true);
    assert(!stream_health_poll(&health, 64999));
    stream_health_expected(&health, 65000, false);
    assert(!stream_health_poll(&health, 100000));
    stream_health_expected(&health, 100000, true);
    assert(!stream_health_poll(&health, 104999));
    assert(stream_health_poll(&health, 105000));
    stream_health_subscription(&health, 105001, false);
    assert(!stream_health_poll(&health, 200000));

    /* Disconnect/reconnect clears stale subscription state and gets fresh grace. */
    start(0);
    assert(stream_health_poll(&health, 5000));
    stream_health_connection(&health, 6000, false, 0x08);
    assert(health.disconnect_reason == 0x08);
    assert(!health.subscribed);
    assert(!stream_health_poll(&health, 100000));
    stream_health_connection(&health, 100000, true, 0);
    assert(!stream_health_poll(&health, 200000));
    stream_health_subscription(&health, 200000, true);
    assert(!stream_health_poll(&health, 204999));

    /* The 32-bit uptime rolls over after roughly 49.7 days. */
    start(UINT32_MAX - 1000U);
    assert(!stream_health_poll(&health, 3998));
    assert(stream_health_poll(&health, 3999));
    assert(health.stalled_stage == STREAM_MIC);

    /* Repeated busy errors count, but do not evict the event history. */
    start(0);
    uint32_t before = health.sequence;
    for (int i = 0; i < 1000; i++) {
        stream_health_error(&health, (uint32_t) i, STREAM_BLE, -ENOMEM);
    }
    assert(health.sequence == before + 1);
    assert(health.error_count == 1000);
    stream_health_progress(&health, 1001, STREAM_BLE);
    stream_health_error(&health, 1002, STREAM_BLE, -ENOMEM);
    assert(health.sequence == before + 2);

    /* Ring overwrites only the oldest events and encodes oldest to newest. */
    start(0);
    for (int i = 1; i <= 40; i++) {
        stream_health_error(&health, (uint32_t) i, STREAM_QUEUE, -i);
    }
    assert(health.event_count == STREAM_HEALTH_EVENT_CAPACITY);
    uint8_t wire[STREAM_HEALTH_WIRE_SIZE + 1];
    memset(wire, 0xA5, sizeof(wire));
    stream_health_encode(&health, 100, wire);
    assert(wire[STREAM_HEALTH_WIRE_SIZE] == 0xA5);
    assert(memcmp(wire, "OMID", 4) == 0);
    assert(wire[4] == 1 && wire[7] == 16);
    assert(wire[64] == health.sequence - 15U);
    assert(wire[64 + 15 * 16] == health.sequence);
    puts("stream health behavioral tests passed");
    return 0;
}
