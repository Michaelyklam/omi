#include "audio_queue.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

static struct audio_queue queue;
static struct audio_frame frame;
static uint8_t payload[AUDIO_TX_FRAME_MAX];
static void make_payload(uint16_t sequence, size_t size) {
    for (size_t i = 0; i < size; ++i) payload[i] = (sequence * 37U + i) & 255;
}
static void check_frame(uint16_t sequence, size_t size) {
    assert(audio_queue_get(&queue, &frame));
    assert(frame.sequence == sequence && frame.size == size);
    make_payload(sequence, size);
    assert(memcmp(frame.data, payload, size) == 0);
}
int main(void) {
    audio_queue_reset(&queue);
    assert(!audio_queue_get(&queue, &frame));
    assert(!audio_queue_put(&queue, payload, 0, 0));
    assert(!audio_queue_put(&queue, payload, AUDIO_TX_FRAME_MAX + 1, 0));
    /* Fill past the old 32-frame limit with complete 40-byte encoded frames. */
    for (unsigned i = 0; i < 500; ++i) {
        make_payload(i, 40);
        assert(audio_queue_put(&queue, payload, 40, i));
    }
    for (unsigned i = 0; i < 500; ++i) check_frame(i, 40);
    assert(!queue.used);
    /* Random variable frames cross both header and payload ring boundaries.
     * Verify every accepted packet against a separate FIFO oracle. */
    unsigned ids[1024], sizes[1024], head = 0, tail = 0;
    srand(19);
    for (unsigned i = 0; i < 200000; ++i) {
        if ((rand() & 3) && tail - head < 1024) {
            size_t size = 1 + rand() % AUDIO_TX_FRAME_MAX;
            make_payload(i, size);
            size_t before = queue.used;
            if (audio_queue_put(&queue, payload, size, i)) {
                ids[tail % 1024] = (uint16_t)i;
                sizes[tail++ % 1024] = size;
            } else assert(queue.used == before);
        } else if (head != tail) {
            check_frame(ids[head % 1024], sizes[head % 1024]);
            ++head;
        }
    }
    while (head != tail) {
        check_frame(ids[head % 1024], sizes[head % 1024]);
        ++head;
    }
    assert(queue.used == 0);
    /* Full queues reject new frames without corrupting older ones. A dropped
     * frame's allocated sequence is observable once a later frame arrives. */
    audio_queue_reset(&queue);
    unsigned accepted = 0;
    for (; accepted < 1000; ++accepted) {
        make_payload(accepted, 320);
        if (!audio_queue_put(&queue, payload, 320, accepted)) break;
    }
    assert(accepted == AUDIO_TX_QUEUE_BYTES / 324);
    check_frame(0, 320);
    make_payload(accepted + 1, 320);
    assert(audio_queue_put(&queue, payload, 320, accepted + 1));
    for (unsigned i = 1; i < accepted; ++i) check_frame(i, 320);
    check_frame(accepted + 1, 320);
    /* Disconnect reset prevents replay into a new connection; wrap IDs are
     * stored verbatim and can still show later dropped frames. */
    make_payload(65535, 73);
    assert(audio_queue_put(&queue, payload, 73, 65535));
    audio_queue_reset(&queue);
    assert(!audio_queue_get(&queue, &frame));
    make_payload(0, 73);
    assert(audio_queue_put(&queue, payload, 73, 0));
    check_frame(0, 73);
    puts("audio queue: capacity, variable FIFO, byte wrap, ID wrap, overflow and session reset passed");
}
