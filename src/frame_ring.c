#include "frame_ring.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <errno.h>

int frame_ring_init(frame_ring_t *ring) {
    memset(ring, 0, sizeof(*ring));
    if (pthread_mutex_init(&ring->lock, NULL) != 0) return -1;
    if (pthread_cond_init(&ring->cond, NULL) != 0) {
        pthread_mutex_destroy(&ring->lock);
        return -1;
    }

    for (int i = 0; i < MAX_RING_BUFFERS; i++) {
        ring->frames[i].data = malloc(MAX_FRAME_SIZE);
        if (!ring->frames[i].data) {
            frame_ring_cleanup(ring);
            return -1;
        }
        ring->frames[i].size = 0;
        ring->frames[i].sequence = 0;
    }
    return 0;
}

void frame_ring_cleanup(frame_ring_t *ring) {
    pthread_mutex_lock(&ring->lock);
    for (int i = 0; i < MAX_RING_BUFFERS; i++) {
        if (ring->frames[i].data) {
            free(ring->frames[i].data);
            ring->frames[i].data = NULL;
        }
    }
    pthread_mutex_unlock(&ring->lock);
    pthread_mutex_destroy(&ring->lock);
    pthread_cond_destroy(&ring->cond);
}

void frame_ring_push(frame_ring_t *ring, const uint8_t *data, size_t size) {
    if (size > MAX_FRAME_SIZE) {
        fprintf(stderr, "Warning: frame size %zu exceeds buffer size %d\n", size, MAX_FRAME_SIZE);
        size = MAX_FRAME_SIZE;
    }

    pthread_mutex_lock(&ring->lock);
    int idx = ring->write_index;
    memcpy(ring->frames[idx].data, data, size);
    ring->frames[idx].size = size;
    ring->total_frames++;
    ring->frames[idx].sequence = ring->total_frames;

    struct timeval tv;
    gettimeofday(&tv, NULL);
    ring->frames[idx].timestamp_us = (uint64_t)tv.tv_sec * 1000000ULL + tv.tv_usec;

    ring->write_index = (ring->write_index + 1) % MAX_RING_BUFFERS;
    pthread_cond_broadcast(&ring->cond);
    pthread_mutex_unlock(&ring->lock);
}

int frame_ring_get_latest(frame_ring_t *ring, uint64_t last_seq, uint8_t *out_buf, size_t out_max, size_t *out_size, uint64_t *out_seq, int timeout_ms) {
    pthread_mutex_lock(&ring->lock);

    struct timespec ts;
    struct timeval tv;
    gettimeofday(&tv, NULL);
    ts.tv_sec = tv.tv_sec + timeout_ms / 1000;
    ts.tv_nsec = (tv.tv_usec + (timeout_ms % 1000) * 1000) * 1000;
    if (ts.tv_nsec >= 1000000000) {
        ts.tv_sec += 1;
        ts.tv_nsec -= 1000000000;
    }

    while (ring->total_frames <= last_seq) {
        int r = pthread_cond_timedwait(&ring->cond, &ring->lock, &ts);
        if (r == ETIMEDOUT) {
            pthread_mutex_unlock(&ring->lock);
            return -1;
        }
    }

    // Latest frame is index (write_index - 1 + MAX) % MAX
    int latest_idx = (ring->write_index - 1 + MAX_RING_BUFFERS) % MAX_RING_BUFFERS;
    frame_t *f = &ring->frames[latest_idx];

    if (f->size > out_max) {
        pthread_mutex_unlock(&ring->lock);
        return -2;
    }

    memcpy(out_buf, f->data, f->size);
    *out_size = f->size;
    *out_seq = f->sequence;

    pthread_mutex_unlock(&ring->lock);
    return 0;
}
