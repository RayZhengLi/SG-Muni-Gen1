// File: ringbuffer.c
#define _POSIX_C_SOURCE 200809L

#include "sg-ringbuffer.h"
#include <stdlib.h>
#include <time.h>
#include <errno.h>

RingBuffer *rb_create(size_t cap) {
    RingBuffer *rb = calloc(1, sizeof(*rb));
    if (!rb) return NULL;
    rb->buf = calloc(cap, sizeof(void*));
    if (!rb->buf) { free(rb); return NULL; }
    rb->cap = cap;
    pthread_mutex_init(&rb->mtx, NULL);
    pthread_cond_init(&rb->not_empty, NULL);
    pthread_cond_init(&rb->not_full, NULL);
    return rb;
}

void rb_destroy(RingBuffer *rb) {
    if (!rb) return;
    free(rb->buf);
    pthread_mutex_destroy(&rb->mtx);
    pthread_cond_destroy(&rb->not_empty);
    pthread_cond_destroy(&rb->not_full);
    free(rb);
}

void rb_push(RingBuffer *rb, void *item) {
    pthread_mutex_lock(&rb->mtx);
    while (rb->cnt == rb->cap)
        pthread_cond_wait(&rb->not_full, &rb->mtx);
    rb->buf[rb->tail] = item;
    rb->tail = (rb->tail + 1) % rb->cap;
    rb->cnt++;
    pthread_cond_signal(&rb->not_empty);
    pthread_mutex_unlock(&rb->mtx);
}

int rb_timed_push(RingBuffer *rb, void *item, int timeout_ms) {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    ts.tv_sec  += timeout_ms / 1000;
    ts.tv_nsec += (timeout_ms % 1000) * 1000000;
    if (ts.tv_nsec >= 1000000000) {
        ts.tv_sec++;
        ts.tv_nsec -= 1000000000;
    }

    pthread_mutex_lock(&rb->mtx);
    while (rb->cnt == rb->cap) {
        int r = pthread_cond_timedwait(&rb->not_full, &rb->mtx, &ts);
        if (r == ETIMEDOUT) {
            pthread_mutex_unlock(&rb->mtx);
            return 0; // timeout
        }
    }
    rb->buf[rb->tail] = item;
    rb->tail = (rb->tail + 1) % rb->cap;
    rb->cnt++;
    pthread_cond_signal(&rb->not_empty);
    pthread_mutex_unlock(&rb->mtx);
    return 1; // success
}

void *rb_pop(RingBuffer *rb) {
    pthread_mutex_lock(&rb->mtx);
    while (rb->cnt == 0)
        pthread_cond_wait(&rb->not_empty, &rb->mtx);
    void *item = rb->buf[rb->head];
    rb->head = (rb->head + 1) % rb->cap;
    rb->cnt--;
    pthread_cond_signal(&rb->not_full);
    pthread_mutex_unlock(&rb->mtx);
    return item;
}

int rb_timed_pop(RingBuffer *rb, void **item, int timeout_ms) {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    ts.tv_sec  += timeout_ms / 1000;
    ts.tv_nsec += (timeout_ms % 1000) * 1000000;
    if (ts.tv_nsec >= 1000000000) {
        ts.tv_sec++;
        ts.tv_nsec -= 1000000000;
    }

    pthread_mutex_lock(&rb->mtx);
    while (rb->cnt == 0) {
        int r = pthread_cond_timedwait(&rb->not_empty, &rb->mtx, &ts);
        if (r == ETIMEDOUT) {
            pthread_mutex_unlock(&rb->mtx);
            return 0;  // timeout
        }
    }
    *item = rb->buf[rb->head];
    rb->head = (rb->head + 1) % rb->cap;
    rb->cnt--;
    pthread_cond_signal(&rb->not_full);
    pthread_mutex_unlock(&rb->mtx);
    return 1;  // success
}
