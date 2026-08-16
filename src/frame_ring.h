#ifndef FRAME_RING_H
#define FRAME_RING_H

#include <stddef.h>
#include <stdint.h>
#include <pthread.h>

#define MAX_RING_BUFFERS 4
#define MAX_FRAME_SIZE (4 * 1024 * 1024) // max frame size bytes

typedef struct {
    uint8_t *data;
    size_t size;
    uint64_t sequence;
    uint64_t timestamp_us;
} frame_t;

typedef struct {
    frame_t frames[MAX_RING_BUFFERS];
    int write_index;
    uint64_t total_frames;
    pthread_mutex_t lock;
    pthread_cond_t cond;
} frame_ring_t;

int frame_ring_init(frame_ring_t *ring);
void frame_ring_cleanup(frame_ring_t *ring);
void frame_ring_push(frame_ring_t *ring, const uint8_t *data, size_t size);
int frame_ring_get_latest(frame_ring_t *ring, uint64_t last_seq, uint8_t *out_buf, size_t out_max, size_t *out_size, uint64_t *out_seq, int timeout_ms);

#endif // FRAME_RING_H
