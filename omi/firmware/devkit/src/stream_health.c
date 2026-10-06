#include "stream_health.h"

#include <string.h>

static void event(struct stream_health *h, uint32_t now, uint8_t type, uint8_t stage, int32_t detail)
{
    h->events[h->event_head] = (struct stream_event) {
        .sequence = ++h->sequence, .uptime_ms = now, .detail = detail, .type = type, .stage = stage};
    h->event_head = (h->event_head + 1U) % STREAM_HEALTH_EVENT_CAPACITY;
    if (h->event_count < STREAM_HEALTH_EVENT_CAPACITY) {
        h->event_count++;
    }
}

static bool active(const struct stream_health *h)
{
    return h->connected && h->subscribed && h->expected;
}

static void rearm(struct stream_health *h, uint32_t now)
{
    for (size_t i = 0; i < STREAM_STAGE_COUNT; i++) {
        h->last_progress[i] = now;
    }
    h->stalled_stage = -1;
}

void stream_health_init(struct stream_health *h, uint32_t reset_reason)
{
    memset(h, 0, sizeof(*h));
    h->reset_reason = reset_reason;
    h->stalled_stage = -1;
}

void stream_health_connection(struct stream_health *h, uint32_t now, bool connected, uint8_t reason)
{
    h->connected = connected;
    h->subscribed = false;
    if (!connected) {
        h->disconnect_reason = reason;
    }
    rearm(h, now);
    event(h, now, connected ? STREAM_CONNECTED : STREAM_DISCONNECTED, STREAM_STAGE_COUNT, reason);
}

void stream_health_subscription(struct stream_health *h, uint32_t now, bool subscribed)
{
    if (h->subscribed == subscribed) {
        return;
    }
    h->subscribed = subscribed;
    rearm(h, now);
    event(h, now, STREAM_SUBSCRIPTION, STREAM_STAGE_COUNT, subscribed);
}

void stream_health_expected(struct stream_health *h, uint32_t now, bool expected)
{
    if (h->expected == expected) {
        return;
    }
    h->expected = expected;
    rearm(h, now);
    event(h, now, STREAM_EXPECTED, STREAM_STAGE_COUNT, expected);
}

void stream_health_progress(struct stream_health *h, uint32_t now, enum stream_stage stage)
{
    if (stage < 0 || stage >= STREAM_STAGE_COUNT) {
        return;
    }
    h->last_progress[stage] = now;
    h->progress_count[stage]++;
    h->last_error[stage] = 0;
}

void stream_health_error(struct stream_health *h, uint32_t now, enum stream_stage stage, int32_t error)
{
    if (stage < 0 || stage >= STREAM_STAGE_COUNT || error == 0) {
        return;
    }
    h->error_count++;
    /* Preserve transitions without logging the same busy error for every frame. */
    if (h->last_error[stage] != error) {
        h->last_error[stage] = error;
        event(h, now, STREAM_ERROR, stage, error);
    }
}

bool stream_health_poll(struct stream_health *h, uint32_t now)
{
    int8_t stalled = -1;
    uint32_t oldest_age = 0;
    if (active(h)) {
        for (size_t i = 0; i < STREAM_STAGE_COUNT; i++) {
            /* Unsigned subtraction also handles the 32-bit uptime rollover. */
            uint32_t age = now - h->last_progress[i];
            /* A blocked sender can fill the queue later. Keep reporting the
             * earliest observed loss of progress rather than its backpressure.
             */
            if (age >= STREAM_HEALTH_TIMEOUT_MS && (stalled < 0 || age > oldest_age)) {
                stalled = (int8_t) i;
                oldest_age = age;
            }
        }
    }
    if (stalled == h->stalled_stage) {
        return false;
    }
    int8_t previous = h->stalled_stage;
    h->stalled_stage = stalled;
    event(h,
          now,
          stalled < 0 ? STREAM_RESUMED : STREAM_STALLED,
          (uint8_t) (stalled < 0 ? previous : stalled),
          stalled < 0 ? 0 : h->last_error[stalled]);
    return true;
}

static void put32(uint8_t *out, uint32_t value)
{
    for (size_t i = 0; i < 4; i++) {
        out[i] = (uint8_t) (value >> (i * 8U));
    }
}

void stream_health_encode(const struct stream_health *h, uint32_t now, uint8_t *wire)
{
    memset(wire, 0, STREAM_HEALTH_WIRE_SIZE);
    memcpy(wire, "OMID", 4);
    wire[4] = 1; /* Schema version. */
    wire[5] = (h->connected ? 1U : 0U) | (h->subscribed ? 2U : 0U) | (h->expected ? 4U : 0U);
    wire[6] = (uint8_t) h->stalled_stage; /* 255 means no stall. */
    wire[7] = h->event_count;
    put32(wire + 8, now);
    put32(wire + 12, h->reset_reason);
    put32(wire + 16, h->sequence);
    put32(wire + 20, h->error_count);
    wire[24] = h->disconnect_reason;
    put32(wire + 60, h->battery_millivolts);
    for (size_t i = 0; i < STREAM_STAGE_COUNT; i++) {
        put32(wire + 28 + i * 4, h->progress_count[i]);
        put32(wire + 44 + i * 4, now - h->last_progress[i]);
    }
    for (size_t i = 0; i < h->event_count; i++) {
        size_t index =
            (h->event_head + STREAM_HEALTH_EVENT_CAPACITY - h->event_count + i) % STREAM_HEALTH_EVENT_CAPACITY;
        const struct stream_event *e = &h->events[index];
        uint8_t *out = wire + 64 + i * 16;
        put32(out, e->sequence);
        put32(out + 4, e->uptime_ms);
        put32(out + 8, (uint32_t) e->detail);
        out[12] = e->type;
        out[13] = e->stage;
    }
}
