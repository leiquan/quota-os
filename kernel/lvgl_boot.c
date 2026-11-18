/*
 * -append lvgl. Admission and the display/input services live here.
 * Widget code is port/quotaos/lvgl_demo.c. This file does not include LVGL.
 */
#include "quotaos.h"

__attribute__((weak)) int lvgl_demo_run(uint32_t *pixels, uint32_t w, uint32_t h, uint32_t stride)
{
    (void)pixels;
    (void)w;
    (void)h;
    (void)stride;
    printf("[lvgl] demo not linked\n");
    return -1;
}

static int admit_lvgl(void)
{
    uint64_t need = (uint64_t)LVGL_HEAP_BYTES + LVGL_DRAW_BYTES + LVGL_FB_BYTES;
    uint32_t need_kb = (uint32_t)((need + 1023u) / 1024u);
    uint64_t cap_kb = resource_app_mem_capacity_kb();
    printf("[admit] lvgl heap %u KB draw %u KB framebuffer %u KB, fixed, pool expand 0\n",
           LVGL_HEAP_BYTES / 1024u, LVGL_DRAW_BYTES / 1024u, LVGL_FB_BYTES / 1024u);
    struct app_manifest too_big = {"lvgl-too-big", "probe", 0xffffffffu, 0xffffffffu, 0, 0};
    int refused = resource_check(&too_big);
    printf("[admit] oversubscription probe %d\n", refused);
    if (refused == ADMIT_OK) {
        printf("[admit] probe was accepted; refusing to start\n");
        return -1;
    }
    printf("%s", resource_reason());
    if ((uint64_t)need_kb > cap_kb) {
        printf("[admit] lvgl reject %u KB > app capacity %lu KB\n", need_kb, cap_kb);
        return -1;
    }
    struct app_manifest m = {"lvgl-demo", "tier-3 ui", need_kb, need_kb, 0, 0};
    if (!resource_commit(&m)) {
        printf("[admit] lvgl-demo refused\n%s", resource_reason());
        return -1;
    }
    printf("[admit] lvgl-demo accepted %u KB. cpu_min is 0. The shell stays tier 3.\n", need_kb);
    return 0;
}

void lvgl_autostart(void)
{
    printf("[lvgl] autostart on the Tier-3 shell task\n");
    printf("[lvgl] this task does not mask IRQs and it is not a tier-1 slot.\n");
    printf("[lvgl] one kernel task. It is not an EL0 process. Three-tier hardware isolation is not claimed.\n");
    if (admit_lvgl() != 0)
        return;
    uint32_t w = 0, h = 0;
    fb_geometry(&w, &h);
    input_set_span(w, h);
    if (fb_present() != 0) {
        printf("[lvgl] ramfb was not published\n");
        return;
    }
    int in = input_init();
    uint64_t start = 0, bytes = 0;
    if (fb_reserved_range(&start, &bytes) != 0)
        return;
    int rc = lvgl_demo_run((uint32_t *)start, w, h, w * 4);
    printf("[lvgl] demo returned %d input_init %d drops %u\n", rc, in, input_drop_count());
}
