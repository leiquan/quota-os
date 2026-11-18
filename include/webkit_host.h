/*
 * Entry the real WebKit engine would use once it is linked.
 * pixels is a kernel-owned XRGB8888 surface (system reserve), not an
 * admitted application heap. heap is NULL until a contiguous allocation
 * can be admitted without taking Tier-1 cache colors.
 * poll_key reports a virtio-input EV_KEY (Linux evdev code). It does not
 * construct WebCore::PlatformKeyboardEvent.
 */
#pragma once
#include <stdint.h>

struct webkit_host {
    uint32_t *pixels;
    uint32_t width;
    uint32_t height;
    uint32_t stride_bytes;
    uint64_t (*now_us)(void);
    int (*poll_key)(uint16_t *code, uint16_t *value);
    void *heap;
    uint64_t heap_bytes;
};

int webkit_engine_linked(void);
int webkit_engine_main(struct webkit_host *host);
