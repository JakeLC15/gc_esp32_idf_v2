#include "ringbuffer.h"
#include <string.h>
#include "esp_heap_caps.h"
#include "esp_log.h"

#define TAG "RING"
#define RING_SIZE (512 * 1024)      // Must be power of two //512 worked

static uint8_t *buffer = NULL;

static volatile uint32_t write_pos = 0;
static volatile uint32_t read_pos  = 0;

void ringbuffer_init(void)
{
    if (buffer)
        return;

    buffer = (uint8_t *)heap_caps_malloc(RING_SIZE, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT); //MALLOC_CAP_SPIRAM);

    if (!buffer)
    {
        ESP_LOGE(TAG, "Failed to allocate ringbuffer");
        return;
    }

    memset(buffer, 0, RING_SIZE);

    write_pos = 0;
    read_pos = 0;

    ESP_LOGI(TAG, "Lock-free ringbuffer %u KB", RING_SIZE / 1024);
}

void ringbuffer_reset(void)
{
    read_pos = 0;
    write_pos = 0;
}

size_t ringbuffer_capacity(void)
{
    return RING_SIZE;
}

size_t ringbuffer_available(void)
{
    uint32_t w = __atomic_load_n(&write_pos, __ATOMIC_ACQUIRE);
    uint32_t r = __atomic_load_n(&read_pos,  __ATOMIC_ACQUIRE);

    return (w >= r) ? (w - r)
                    : (RING_SIZE - r + w);
}

size_t ringbuffer_free(void)
{
    return (RING_SIZE - 1) - ringbuffer_available();
}

void ringbuffer_write(const void *data, size_t len)
{
    if (!buffer || !data || !len)
        return;

    size_t free = ringbuffer_free();


    if (len > free)
        return;

    uint32_t w = __atomic_load_n(&write_pos, __ATOMIC_RELAXED);
    //uint32_t w = write_pos;

    size_t first = RING_SIZE - w;

    if (first > len)
        first = len;

    memcpy(buffer + w, data, first);

    if (len > first)
        memcpy(buffer, (const uint8_t *)data + first, len - first);

    // Publish after data is copied
    __atomic_store_n(&write_pos,
                     (w + len) & (RING_SIZE - 1),
                     __ATOMIC_RELEASE);
}

size_t ringbuffer_read(void *out, size_t len)
{
    if (!buffer || !out || !len)
        return 0;
    
    uint32_t r = read_pos;
    uint32_t w = __atomic_load_n(&write_pos, __ATOMIC_ACQUIRE);

    size_t avail;

    if (w >= r)
        avail = w - r;
    else
        avail = RING_SIZE - r + w;

    if (avail == 0)
    {
        memset(out, 0, len);
        return 0;
    }

    size_t got = (avail < len) ? avail : len;
    size_t first = RING_SIZE - r;

    if (first > got)
        first = got;

    memcpy(out, buffer + r, first);

    if (got > first)
        memcpy((uint8_t *)out + first, buffer, got - first);

    __atomic_store_n(&read_pos,
                     (r + got) & (RING_SIZE - 1),
                     __ATOMIC_RELEASE);

    if (got < len)
        memset((uint8_t *)out + got, 0, len - got);

    return got;
}
