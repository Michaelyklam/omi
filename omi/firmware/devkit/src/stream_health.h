#ifndef STREAM_HEALTH_H
#define STREAM_HEALTH_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define STREAM_HEALTH_TIMEOUT_MS 5000U
#define STREAM_HEALTH_EVENT_CAPACITY 16U
#define STREAM_HEALTH_WIRE_SIZE (64U + STREAM_HEALTH_EVENT_CAPACITY * 16U)

enum stream_stage { STREAM_MIC, STREAM_CODEC, STREAM_QUEUE, STREAM_BLE, STREAM_STAGE_COUNT };
enum stream_event_type {
    STREAM_CONNECTED = 1,
    STREAM_DISCONNECTED,
    STREAM_SUBSCRIPTION,
    STREAM_ERROR,
    STREAM_STALLED,
    STREAM_RESUMED,
    STREAM_EXPECTED
};

struct stream_event {
    uint32_t sequence;
    uint32_t uptime_ms;
    int32_t detail;
    uint8_t type;
    uint8_t stage;
};

/* Serialized by diagnostics.c, never copied directly into the BLE wire format.
 * Callers must serialize access; the firmware adapter uses an IRQ-safe spinlock.
 */
struct stream_health {
    uint32_t last_progress[STREAM_STAGE_COUNT];
    uint32_t progress_count[STREAM_STAGE_COUNT];
    int32_t last_error[STREAM_STAGE_COUNT];
    struct stream_event events[STREAM_HEALTH_EVENT_CAPACITY];
    uint32_t sequence;
    uint32_t error_count;
    uint32_t reset_reason;
    uint32_t battery_millivolts;
    uint8_t event_head;
    uint8_t event_count;
    uint8_t disconnect_reason;
    int8_t stalled_stage;
    bool connected;
    bool subscribed;
    bool expected;
};

void stream_health_init(struct stream_health *health, uint32_t reset_reason);
void stream_health_connection(struct stream_health *health, uint32_t now, bool connected, uint8_t reason);
void stream_health_subscription(struct stream_health *health, uint32_t now, bool subscribed);
void stream_health_expected(struct stream_health *health, uint32_t now, bool expected);
void stream_health_progress(struct stream_health *health, uint32_t now, enum stream_stage stage);
void stream_health_error(struct stream_health *health, uint32_t now, enum stream_stage stage, int32_t error);
/* Returns true only when the fault classification changes. No automatic reset. */
bool stream_health_poll(struct stream_health *health, uint32_t now);
void stream_health_encode(const struct stream_health *health, uint32_t now, uint8_t *wire);

#endif
