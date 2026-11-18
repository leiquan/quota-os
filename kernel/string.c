#include "quotaos.h"

/* 对齐部分按 8 字节写：内核在周期边界清演示统计，逐字节写太慢。 */
__attribute__((optimize("no-tree-loop-distribute-patterns")))
void *memset(void *dst, int c, size_t n)
{
    uint8_t *p = dst;
    while (n && ((uintptr_t)p & 7)) {
        *p++ = (uint8_t)c;
        n--;
    }
    uint64_t w = (uint8_t)c * 0x0101010101010101ull;
    for (; n >= 8; n -= 8, p += 8)
        *(uint64_t *)p = w;
    while (n--)
        *p++ = (uint8_t)c;
    return dst;
}

int strcmp(const char *a, const char *b)
{
    while (*a && *a == *b) {
        a++;
        b++;
    }
    return (int)(unsigned char)*a - (int)(unsigned char)*b;
}

/* 复制到定长缓冲区，总是以 0 结尾。 */
void str_copy(char *dst, const char *src, int cap)
{
    int i = 0;
    for (; i < cap - 1 && src && src[i]; i++)
        dst[i] = src[i];
    dst[i] = 0;
}

__attribute__((optimize("no-tree-loop-distribute-patterns")))
void *memcpy(void *dst, const void *src, size_t n)
{
    uint8_t *d = dst;
    const uint8_t *s = src;
    while (n--)
        *d++ = *s++;
    return dst;
}
