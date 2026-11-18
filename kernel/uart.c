/*
 * QEMU virt 的 PL011，物理地址 0x09000000。
 * 控制台是这个教学系统唯一的人机界面：菜单、准入询问和演示输出都走这里。
 */
#include "quotaos.h"

#define UART_BASE 0x09000000ul
#define UART_DR   (UART_BASE + 0x00)
#define UART_FR   (UART_BASE + 0x18)
#define UART_IBRD (UART_BASE + 0x24)
#define UART_FBRD (UART_BASE + 0x28)
#define UART_LCRH (UART_BASE + 0x2c)
#define UART_CR   (UART_BASE + 0x30)
#define UART_ICR  (UART_BASE + 0x44)

void uart_init(void)
{
    mmio_wr(UART_CR, 0);
    mmio_wr(UART_ICR, 0x7ff);
    /* 24 MHz / (16 * 115200) ≈ 13.02 */
    mmio_wr(UART_IBRD, 13);
    mmio_wr(UART_FBRD, 1);
    mmio_wr(UART_LCRH, (3u << 5) | (1u << 4));
    mmio_wr(UART_CR, (1u << 0) | (1u << 8) | (1u << 9));
}

void uart_putc(char c)
{
    if (c == '\n')
        uart_putc('\r');
    while (mmio_rd(UART_FR) & (1u << 5))
        ;
    mmio_wr(UART_DR, (uint32_t)c);
}

static int uart_getc_nonblock(void)
{
    if (mmio_rd(UART_FR) & (1u << 4))
        return -1;
    return (int)(mmio_rd(UART_DR) & 0xff);
}

static char uart_getc(void)
{
    int c;
    for (;;) {
        c = uart_getc_nonblock();
        if (c >= 0)
            return (char)c;
        asm volatile("wfi");
    }
}

void readline(char *buf, int cap)
{
    int n = 0;
    if (cap < 2) {
        if (cap > 0)
            buf[0] = 0;
        return;
    }
    for (;;) {
        char c = uart_getc();
        if (c == '\r' || c == '\n') {
            uart_putc('\n');
            break;
        }
        if (c == 0x7f || c == '\b') {
            if (n > 0) {
                n--;
                printf("\b \b");
            }
            continue;
        }
        if (c < 32)
            continue;
        if (n < cap - 1) {
            buf[n++] = c;
            uart_putc(c);
        }
    }
    buf[n] = 0;
}

__attribute__((weak)) void mmu_init(void) {}
__attribute__((weak)) int mmu_protect(uint64_t addr, uint64_t len, int prot)
{
    (void)addr;
    (void)len;
    (void)prot;
    return -1;
}
__attribute__((weak)) int mmu_fault_expected(void) { return 0; }
__attribute__((weak)) void mmu_note_fault(void) {}
