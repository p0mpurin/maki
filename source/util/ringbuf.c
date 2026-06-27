#include "ringbuf.h"
#include <stdlib.h>
#include <string.h>

RingBuffer *ringbuf_create(size_t capacity) {
    RingBuffer *rb = (RingBuffer *)malloc(sizeof(RingBuffer));
    if (!rb) return NULL;
    rb->buf = (uint8_t *)malloc(capacity);
    if (!rb->buf) {
        free(rb);
        return NULL;
    }
    rb->capacity = capacity;
    rb->read_pos = 0;
    rb->write_pos = 0;
    rb->used = 0;
    LightLock_Init(&rb->lock);
    return rb;
}

void ringbuf_destroy(RingBuffer *rb) {
    if (!rb) return;
    free(rb->buf);
    free(rb);
}

size_t ringbuf_write(RingBuffer *rb, const uint8_t *data, size_t len) {
    if (!rb || !data || len == 0) return 0;
    LightLock_Lock(&rb->lock);
    size_t space = rb->capacity - rb->used;
    if (len > space) len = space;
    size_t written = 0;
    while (written < len) {
        size_t chunk = len - written;
        size_t tail = rb->capacity - rb->write_pos;
        if (chunk > tail) chunk = tail;
        memcpy(rb->buf + rb->write_pos, data + written, chunk);
        rb->write_pos = (rb->write_pos + chunk) % rb->capacity;
        rb->used += chunk;
        written += chunk;
    }
    LightLock_Unlock(&rb->lock);
    return written;
}

size_t ringbuf_read(RingBuffer *rb, uint8_t *out, size_t len) {
    if (!rb || !out || len == 0) return 0;
    LightLock_Lock(&rb->lock);
    if (rb->used == 0) {
        LightLock_Unlock(&rb->lock);
        return 0;
    }
    if (len > rb->used) len = rb->used;
    size_t read = 0;
    while (read < len) {
        size_t chunk = len - read;
        size_t tail = rb->capacity - rb->read_pos;
        if (chunk > tail) chunk = tail;
        memcpy(out + read, rb->buf + rb->read_pos, chunk);
        rb->read_pos = (rb->read_pos + chunk) % rb->capacity;
        rb->used -= chunk;
        read += chunk;
    }
    LightLock_Unlock(&rb->lock);
    return read;
}

size_t ringbuf_used(RingBuffer *rb) {
    if (!rb) return 0;
    LightLock_Lock(&rb->lock);
    size_t u = rb->used;
    LightLock_Unlock(&rb->lock);
    return u;
}

size_t ringbuf_free(RingBuffer *rb) {
    if (!rb) return 0;
    LightLock_Lock(&rb->lock);
    size_t f = rb->capacity - rb->used;
    LightLock_Unlock(&rb->lock);
    return f;
}

void ringbuf_clear(RingBuffer *rb) {
    if (!rb) return;
    LightLock_Lock(&rb->lock);
    rb->read_pos = 0;
    rb->write_pos = 0;
    rb->used = 0;
    LightLock_Unlock(&rb->lock);
}
