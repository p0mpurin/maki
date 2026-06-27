#ifndef RINGBUF_H
#define RINGBUF_H

#include <stddef.h>
#include <stdint.h>
#include <3ds.h>

typedef struct {
    uint8_t *buf;
    size_t   capacity;
    size_t   read_pos;
    size_t   write_pos;
    size_t   used;
    LightLock lock;
} RingBuffer;

RingBuffer *ringbuf_create(size_t capacity);
void        ringbuf_destroy(RingBuffer *rb);
size_t      ringbuf_write(RingBuffer *rb, const uint8_t *data, size_t len);
size_t      ringbuf_read(RingBuffer *rb, uint8_t *out, size_t len);
size_t      ringbuf_used(RingBuffer *rb);
size_t      ringbuf_free(RingBuffer *rb);
void        ringbuf_clear(RingBuffer *rb);

#endif
