/*
 * 解析 QEMU virt 设备树：memory 节点给出这台“设备”有多少 RAM，
 * interrupt-controller 节点给出 GIC 的版本和地址。
 * 旧机器场景（-m 64M）和 128M 走的是同一段代码，差别只来自这里读到的数字。
 */
#include "quotaos.h"

static volatile int expect_fault;
static const uint8_t *dtb_base;
static volatile int fault_hit;

int dtb_fault_expected(void)
{
    return expect_fault;
}

void dtb_note_fault(void)
{
    fault_hit = 1;
    expect_fault = 0;
}

static int probe32(uint64_t addr, uint32_t *out)
{
    uint32_t v = 0;
    fault_hit = 0;
    expect_fault = 1;
    asm volatile("ldr %w0, [%1]" : "=r"(v) : "r"(addr) : "memory");
    int hit = fault_hit;
    expect_fault = 0;
    if (hit)
        return -1;
    *out = v;
    return 0;
}

static uint64_t detect_ram_span(void)
{
    uint64_t base = 0x40000000ull;
    uint64_t size = 0;
    for (uint64_t mb = 16; mb <= 2048; mb += 16) {
        uint32_t dummy;
        uint64_t addr = base + mb * 1024ull * 1024ull - 4ull;
        if (probe32(addr, &dummy) != 0)
            break;
        size = mb * 1024ull * 1024ull;
    }
    if (size == 0)
        size = 128ull << 20;
    return size;
}

static uint32_t be32_at(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

/*
 * color_init 会写每个 Tier 1 页的第一个字。QEMU 8.2 把 DTB 放在页池里
 * （64M 时在 0x42000000），那一笔写进结构体中间，后面的 GIC 查找就失败。
 * 在着色之前把真正用到的字节拷到内核 BSS。系统预留本来就盖住这块 BSS，
 * 不改准入数字。
 */
static uint8_t dtb_copy[65536];

static void snapshot_dtb(void)
{
    const uint8_t *dtb = dtb_base;
    if (be32_at(dtb) != 0xd00dfeedu)
        return;
    uint32_t totalsize = be32_at(dtb + 4);
    uint32_t version = be32_at(dtb + 20);
    uint32_t end = totalsize;
    if (version >= 17) {
        uint32_t struct_end = be32_at(dtb + 8) + be32_at(dtb + 36);
        uint32_t strings_end = be32_at(dtb + 12) + be32_at(dtb + 32);
        end = struct_end > strings_end ? struct_end : strings_end;
    }
    end = (end + 3u) & ~3u;
    if (end < 40 || end > totalsize || end > sizeof dtb_copy) {
        printf("[boot] DTB snapshot skipped, need %u bytes\n", end);
        return;
    }
    memcpy(dtb_copy, dtb, end);
    dtb_copy[4] = (uint8_t)(end >> 24);
    dtb_copy[5] = (uint8_t)(end >> 16);
    dtb_copy[6] = (uint8_t)(end >> 8);
    dtb_copy[7] = (uint8_t)end;
    dtb_base = dtb_copy;
    printf("[boot] DTB snapshot %u bytes (header said %u)\n", end, totalsize);
}

static uint64_t parse_memory(const uint8_t *dtb)
{
    uint32_t off_struct = be32_at(dtb + 8);
    uint32_t off_strings = be32_at(dtb + 12);
    uint32_t totalsize = be32_at(dtb + 4);
    const uint8_t *p = dtb + off_struct;
    const uint8_t *end = dtb + totalsize;
    const uint8_t *strings = dtb + off_strings;
    int ac = 2, sc = 2, depth = 0, in_mem = 0;
    uint64_t ram = 0;

    while (p + 4 <= end) {
        uint32_t tok = be32_at(p);
        p += 4;
        if (tok == 1) {
            const uint8_t *s = p;
            while (p < end && *p)
                p++;
            if (p < end)
                p++;
            p = (const uint8_t *)(((uintptr_t)p + 3u) & ~(uintptr_t)3u);
            depth++;
            in_mem = (depth > 1 && s[0] == 'm' && s[1] == 'e' && s[2] == 'm');
        } else if (tok == 2) {
            if (depth > 0)
                depth--;
            in_mem = 0;
        } else if (tok == 3) {
            if (p + 8 > end)
                break;
            uint32_t len = be32_at(p);
            uint32_t nameoff = be32_at(p + 4);
            p += 8;
            if (p + len > end)
                break;
            const char *name = (const char *)(strings + nameoff);
            if (depth == 1 && name[0] == '#' && name[1] == 'a' && len >= 4) {
                ac = (int)be32_at(p);
            } else if (depth == 1 && name[0] == '#' && name[1] == 's' && len >= 4) {
                sc = (int)be32_at(p);
            } else if (in_mem && name[0] == 'r' && name[1] == 'e' && name[2] == 'g') {
                int cells = ac + sc;
                int n = cells ? (int)(len / 4) / cells : 0;
                const uint8_t *r = p;
                for (int i = 0; i < n; i++) {
                    uint64_t sz = 0;
                    const uint8_t *scell = r + (ac * 4);
                    for (int k = 0; k < sc; k++)
                        sz = (sz << 32) | be32_at(scell + k * 4);
                    ram += sz;
                    r += cells * 4;
                }
            }
            p += (len + 3u) & ~3u;
        } else if (tok == 4) {
            continue;
        } else {
            break;
        }
    }
    return ram;
}

uint64_t dtb_ram_bytes(uint64_t hinted)
{
    uint64_t span = detect_ram_span();
    uint64_t dtb = 0;
    printf("[boot] x0=0x%lx physical RAM span %lu MB (probed)\n", hinted, span >> 20);

    if (hinted >= 0x40000000ull && hinted + 8 < 0x40000000ull + span) {
        uint32_t magic = *(volatile uint32_t *)hinted;
        if (magic == 0xedfe0dd0u)
            dtb = hinted;
    }

    if (!dtb) {
        for (uint64_t p = 0x40000000ull; p + 64 < 0x40000000ull + span; p += 0x1000) {
            uint32_t magic = *(volatile uint32_t *)p;
            if (magic == 0xedfe0dd0u) {
                dtb = p;
                break;
            }
        }
    }

    if (!dtb) {
        printf("[boot] DTB not found, using probed RAM size\n");
        return span;
    }

    printf("[boot] DTB at 0x%lx\n", dtb);
    dtb_base = (const uint8_t *)dtb;
    snapshot_dtb();
    uint64_t ram = parse_memory(dtb_base);
    if (ram == 0) {
        printf("[boot] DTB has no memory node, using probed size\n");
        return span;
    }
    printf("[boot] DTB memory %lu bytes\n", ram);
    return ram;
}

static int str_in_list(const char *list, uint32_t len, const char *want)
{
    uint32_t i = 0;
    while (i < len) {
        const char *s = list + i;
        if (strcmp(s, want) == 0)
            return 1;
        while (i < len && list[i])
            i++;
        i++;
    }
    return 0;
}

/*
 * 找第一个 compatible 里含 want 的节点，按父节点的 #address-cells / #size-cells
 * 把 reg 解成 (base, size) 对写进 out。返回写入的个数，找不到返回 0。
 */
int dtb_find_compatible(const char *want, uint64_t *out, int max)
{
    if (!dtb_base)
        return 0;
    const uint8_t *dtb = dtb_base;
    const uint8_t *p = dtb + be32_at(dtb + 8);
    const uint8_t *end = dtb + be32_at(dtb + 4);
    const char *strings = (const char *)(dtb + be32_at(dtb + 12));
    enum { MAXD = 16 };
    int ac[MAXD], sc[MAXD], match[MAXD];
    const uint8_t *reg[MAXD];
    uint32_t reg_len[MAXD];
    int depth = 0;

    while (p + 4 <= end) {
        uint32_t tok = be32_at(p);
        p += 4;
        if (tok == 1 || tok == 2) {
            /* 当前节点的属性已经读完（子节点开始或节点结束），看它是不是要找的。 */
            if (depth > 0 && depth < MAXD && match[depth] && reg[depth]) {
                int cells = ac[depth - 1] + sc[depth - 1];
                int n = 0;
                for (uint32_t off = 0; cells && off + (uint32_t)cells * 4 <= reg_len[depth] && n + 2 <= max;
                     off += (uint32_t)cells * 4) {
                    uint64_t base = 0, size = 0;
                    for (int k = 0; k < ac[depth - 1]; k++)
                        base = (base << 32) | be32_at(reg[depth] + off + k * 4);
                    for (int k = 0; k < sc[depth - 1]; k++)
                        size = (size << 32) | be32_at(reg[depth] + off + (ac[depth - 1] + k) * 4);
                    out[n++] = base;
                    out[n++] = size;
                }
                return n;
            }
            if (depth > 0 && depth < MAXD)
                match[depth] = 0;
        }
        if (tok == 1) {
            while (p < end && *p)
                p++;
            p = (const uint8_t *)(((uintptr_t)p + 4u) & ~(uintptr_t)3u);
            depth++;
            if (depth >= MAXD)
                return 0;
            ac[depth] = 2;
            sc[depth] = 1;
            match[depth] = 0;
            reg[depth] = 0;
            reg_len[depth] = 0;
        } else if (tok == 2) {
            if (depth > 0)
                depth--;
        } else if (tok == 3) {
            if (p + 8 > end)
                break;
            uint32_t len = be32_at(p);
            const char *name = strings + be32_at(p + 4);
            p += 8;
            if (p + len > end)
                break;
            if (strcmp(name, "#address-cells") == 0 && len >= 4)
                ac[depth] = (int)be32_at(p);
            else if (strcmp(name, "#size-cells") == 0 && len >= 4)
                sc[depth] = (int)be32_at(p);
            else if (strcmp(name, "compatible") == 0)
                match[depth] = str_in_list((const char *)p, len, want);
            else if (strcmp(name, "reg") == 0) {
                reg[depth] = p;
                reg_len[depth] = len;
            }
            p += (len + 3u) & ~3u;
        } else if (tok == 4) {
            continue;
        } else {
            break;
        }
    }
    return 0;
}

/* 同 dtb_find_compatible，但收集每一个匹配节点的第一段 reg 基址。 */
int dtb_find_bases(const char *want, uint64_t *bases, int max)
{
    if (!dtb_base || max <= 0)
        return 0;
    const uint8_t *dtb = dtb_base;
    const uint8_t *p = dtb + be32_at(dtb + 8);
    const uint8_t *end = dtb + be32_at(dtb + 4);
    const char *strings = (const char *)(dtb + be32_at(dtb + 12));
    enum { MAXD = 16 };
    int ac[MAXD], sc[MAXD], match[MAXD];
    const uint8_t *reg[MAXD];
    uint32_t reg_len[MAXD];
    int depth = 0, n = 0;

    while (p + 4 <= end && n < max) {
        uint32_t tok = be32_at(p);
        p += 4;
        if (tok == 1 || tok == 2) {
            if (depth > 0 && depth < MAXD && match[depth] && reg[depth]) {
                int cells = ac[depth - 1] + sc[depth - 1];
                if (cells && reg_len[depth] >= (uint32_t)cells * 4) {
                    uint64_t base = 0;
                    for (int k = 0; k < ac[depth - 1]; k++)
                        base = (base << 32) | be32_at(reg[depth] + k * 4);
                    bases[n++] = base;
                }
            }
            if (depth > 0 && depth < MAXD)
                match[depth] = 0;
        }
        if (tok == 1) {
            while (p < end && *p)
                p++;
            p = (const uint8_t *)(((uintptr_t)p + 4u) & ~(uintptr_t)3u);
            depth++;
            if (depth >= MAXD)
                return n;
            ac[depth] = 2;
            sc[depth] = 1;
            match[depth] = 0;
            reg[depth] = 0;
            reg_len[depth] = 0;
        } else if (tok == 2) {
            if (depth > 0)
                depth--;
        } else if (tok == 3) {
            if (p + 8 > end)
                break;
            uint32_t len = be32_at(p);
            const char *name = strings + be32_at(p + 4);
            p += 8;
            if (p + len > end)
                break;
            if (strcmp(name, "#address-cells") == 0 && len >= 4)
                ac[depth] = (int)be32_at(p);
            else if (strcmp(name, "#size-cells") == 0 && len >= 4)
                sc[depth] = (int)be32_at(p);
            else if (strcmp(name, "compatible") == 0)
                match[depth] = str_in_list((const char *)p, len, want);
            else if (strcmp(name, "reg") == 0) {
                reg[depth] = p;
                reg_len[depth] = len;
            }
            p += (len + 3u) & ~3u;
        } else if (tok == 4) {
            continue;
        } else {
            break;
        }
    }
    return n;
}

/* /chosen/bootargs（QEMU -append）。没有就返回空串。 */
const char *dtb_bootargs(void)
{
    if (!dtb_base)
        return "";
    const uint8_t *dtb = dtb_base;
    const uint8_t *p = dtb + be32_at(dtb + 8);
    const uint8_t *end = dtb + be32_at(dtb + 4);
    const char *strings = (const char *)(dtb + be32_at(dtb + 12));
    int depth = 0, in_chosen = 0;
    while (p + 4 <= end) {
        uint32_t tok = be32_at(p);
        p += 4;
        if (tok == 1) {
            const char *name = (const char *)p;
            depth++;
            in_chosen = depth == 2 && strcmp(name, "chosen") == 0;
            while (p < end && *p)
                p++;
            p = (const uint8_t *)(((uintptr_t)p + 4u) & ~(uintptr_t)3u);
        } else if (tok == 2) {
            depth--;
            in_chosen = 0;
        } else if (tok == 3) {
            uint32_t len = be32_at(p);
            const char *name = strings + be32_at(p + 4);
            p += 8;
            if (in_chosen && strcmp(name, "bootargs") == 0 && len > 0)
                return (const char *)p;
            p += (len + 3u) & ~3u;
        } else if (tok != 4) {
            break;
        }
    }
    return "";
}
