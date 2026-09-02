#pragma once

#include <stddef.h>
#include <stdint.h>

void ringbuffer_init(void);
void ringbuffer_reset(void);

void ringbuffer_write(const void *data, size_t len);

size_t ringbuffer_read(void *data, size_t len);
size_t ringbuffer_available(void);
size_t ringbuffer_free(void);
size_t ringbuffer_capacity(void);
