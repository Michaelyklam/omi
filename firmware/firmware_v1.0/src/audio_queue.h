#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* About five seconds at 32 kbps, including packet headers. Caller serializes
 * access; neither producer nor consumer waits while holding the queue lock. */
#define AUDIO_TX_QUEUE_BYTES 24576
#define AUDIO_TX_FRAME_MAX 320
struct audio_frame {
    uint16_t sequence;
    uint16_t size;
    uint8_t data[AUDIO_TX_FRAME_MAX];
};
struct audio_queue {
    uint8_t data[AUDIO_TX_QUEUE_BYTES];
    size_t read, write, used;
};
void audio_queue_reset(struct audio_queue *q);
bool audio_queue_put(struct audio_queue *q, const uint8_t *data, size_t size, uint16_t sequence);
bool audio_queue_get(struct audio_queue *q, struct audio_frame *frame);
