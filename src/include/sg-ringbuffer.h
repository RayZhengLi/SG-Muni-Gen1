// File: ringbuffer.h
#ifndef RINGBUFFER_H
#define RINGBUFFER_H

#include <stdlib.h>
#include <pthread.h>

/**
 * 多生产者-多消费者环形队列，内部使用 mutex + cond 实现阻塞。
 */
typedef struct {
    void          **buf;
    size_t          cap, head, tail, cnt;
    pthread_mutex_t mtx;
    pthread_cond_t  not_empty, not_full;
} RingBuffer;

/**
 * 创建一个容量为 cap 的环形队列。
 * 返回 NULL 表示分配失败。
 */
RingBuffer *rb_create(size_t cap);

/**
 * 销毁队列并释放所有内部资源。
 */
void        rb_destroy(RingBuffer *rb);

/**
 * 向队列尾部推送一个元素；如果队列满则阻塞。
 */
void        rb_push(RingBuffer *rb, void *item);

/**
 * Push an element into the ringbuffer with time out;
 * Return 1 for success, 0 for timeout
 */
int         rb_timed_push(RingBuffer *rb, void *item, int timeout_ms);
/**
 * 从队列头部弹出一个元素；如果队列空则阻塞。
 * 返回弹出的元素指针。
 */
void       *rb_pop(RingBuffer *rb);

/**
 * 带超时的弹出接口；返回 1 表示成功，0 表示超时
 */
int rb_timed_pop(RingBuffer *rb, void **item, int timeout_ms);

#endif // RINGBUFFER_H