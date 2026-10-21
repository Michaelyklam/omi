#include "audio_queue.h"
#include <string.h>

void audio_queue_reset(struct audio_queue *q)
{
    q->read = q->write = q->used = 0;
}

static void put(struct audio_queue *q, const uint8_t *data, size_t size)
{
    size_t first = size < AUDIO_TX_QUEUE_BYTES - q->write ? size : AUDIO_TX_QUEUE_BYTES - q->write;
    memcpy(q->data + q->write, data, first);
    memcpy(q->data, data + first, size - first);
    q->write = (q->write + size) % AUDIO_TX_QUEUE_BYTES;
    q->used += size;
}

static void get(struct audio_queue *q, uint8_t *data, size_t size)
{
    size_t first = size < AUDIO_TX_QUEUE_BYTES - q->read ? size : AUDIO_TX_QUEUE_BYTES - q->read;
    memcpy(data, q->data + q->read, first);
    memcpy(data + first, q->data, size - first);
    q->read = (q->read + size) % AUDIO_TX_QUEUE_BYTES;
    q->used -= size;
}

bool audio_queue_put(struct audio_queue *q, const uint8_t *data, size_t size, uint16_t sequence)
{
    if (!size || size > AUDIO_TX_FRAME_MAX || size + 4 > AUDIO_TX_QUEUE_BYTES - q->used) {
        return false; /* No partial frame is ever published. */
    }
    uint8_t header[4] = {size & 255, size >> 8, sequence & 255, sequence >> 8};
    put(q, header, sizeof(header));
    put(q, data, size);
    return true;
}

bool audio_queue_get(struct audio_queue *q, struct audio_frame *frame)
{
    if (!q->used) return false;
    uint8_t header[4];
    get(q, header, sizeof(header));
    frame->size = header[0] | ((uint16_t)header[1] << 8);
    frame->sequence = header[2] | ((uint16_t)header[3] << 8);
    get(q, frame->data, frame->size);
    return true;
}
