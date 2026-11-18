/*
 * LVGL adaptation and the demo screen. Calls the kernel framebuffer and
 * the input queue. Does not program fw_cfg, virtio, or the page tables.
 */
#include "quotaos.h"

#define LV_CONF_INCLUDE_SIMPLE
#include "lvgl.h"

#include <stddef.h>

_Static_assert(LV_MEM_SIZE == LVGL_HEAP_BYTES, "LVGL heap must match the admitted size");
_Static_assert(LV_MEM_POOL_EXPAND_SIZE == 0, "LVGL heap must not grow");

static uint8_t draw_buf[LVGL_DRAW_BYTES] __attribute__((aligned(64)));
static uint32_t *fb_px;
static uint32_t fb_w, fb_h, fb_stride;
static lv_obj_t *count_label;
static lv_obj_t *field;
static lv_obj_t *menu_list;
static lv_obj_t *legend;
static lv_obj_t *detail;
static lv_obj_t *page_home;
static lv_obj_t *page_input;
static lv_obj_t *page_cover;

static void show_page(int which)
{
    if (which == 0) {
        lv_obj_remove_flag(page_home, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(page_input, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(page_cover, LV_OBJ_FLAG_HIDDEN);
    } else if (which == 1) {
        lv_obj_add_flag(page_home, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(page_input, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(page_cover, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(page_home, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(page_input, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(page_cover, LV_OBJ_FLAG_HIDDEN);
    }
}
static unsigned clicks;

#define CUR_W 16
#define CUR_H 16
static uint8_t cursor_px[CUR_W * CUR_H * 4];
static lv_image_dsc_t cursor_dsc;

static void cursor_px_set(int x, int y, uint8_t r, uint8_t g, uint8_t b, uint8_t a)
{
    uint8_t *p = cursor_px + ((y * CUR_W) + x) * 4;
    p[0] = b;
    p[1] = g;
    p[2] = r;
    p[3] = a;
}

/* Tip at (0,0). Black arrow, one-pixel white outline. 1024 bytes, fixed. */
static void build_cursor(void)
{
    static const char *core[CUR_H] = {
        "#...............",
        "##..............",
        "#.#.............",
        "#..#............",
        "#...#...........",
        "#....#..........",
        "#.....#.........",
        "#......#........",
        "#.......#.......",
        "#........#......",
        "#....#####......",
        "#.#.#...........",
        "##..#...........",
        "#...#...........",
        "....#...........",
        "....#...........",
    };
    int black[CUR_H][CUR_W];
    for (int y = 0; y < CUR_H; y++) {
        for (int x = 0; x < CUR_W; x++)
            black[y][x] = core[y][x] == '#';
    }
    for (int y = 0; y < CUR_H; y++) {
        for (int x = 0; x < CUR_W; x++) {
            if (black[y][x]) {
                cursor_px_set(x, y, 0, 0, 0, 255);
                continue;
            }
            int edge = 0;
            for (int dy = -1; dy <= 1 && !edge; dy++) {
                for (int dx = -1; dx <= 1; dx++) {
                    int yy = y + dy, xx = x + dx;
                    if (yy >= 0 && xx >= 0 && yy < CUR_H && xx < CUR_W && black[yy][xx])
                        edge = 1;
                }
            }
            if (edge)
                cursor_px_set(x, y, 255, 255, 255, 255);
            else
                cursor_px_set(x, y, 0, 0, 0, 0);
        }
    }
    cursor_dsc.header.magic = LV_IMAGE_HEADER_MAGIC;
    cursor_dsc.header.cf = LV_COLOR_FORMAT_ARGB8888;
    cursor_dsc.header.flags = 0;
    cursor_dsc.header.w = CUR_W;
    cursor_dsc.header.h = CUR_H;
    cursor_dsc.header.stride = CUR_W * 4;
    cursor_dsc.data_size = sizeof cursor_px;
    cursor_dsc.data = cursor_px;
}

static struct quota_input keys[32];
static struct quota_input ptrs[32];
static int nkeys, nptrs, key_i, ptr_i;

void lvgl_port_assert(void)
{
    printf("[lvgl] assert\n");
    for (;;)
        asm volatile("wfi");
}

static uint32_t lvgl_tick_ms(void)
{
    return (uint32_t)(now_us() / 1000ull);
}

enum { SAMP_N = 16 };
static uint64_t click_stamp, key_stamp, scroll_stamp;
static uint64_t click_dt[SAMP_N], key_dt[SAMP_N], scroll_dt[SAMP_N];
static int n_click, n_key, n_scroll;
static uint32_t flush_n, flush_max_bytes;

static void take_dt(uint64_t *stamp, uint64_t *arr, int *n, const char *kind)
{
    uint64_t d;
    if (!*stamp)
        return;
    d = now_us() - *stamp;
    *stamp = 0;
    if (*n < SAMP_N)
        arr[(*n)++] = d;
    printf("[lvgl] fb commit %s %lu us n=%d (not panel light)\n", kind, (unsigned long)d, *n);
}

static void flush_cb(lv_display_t *disp, const lv_area_t *area, uint8_t *px)
{
    int32_t w = lv_area_get_width(area);
    int32_t h = lv_area_get_height(area);
    uint32_t stride_px = fb_stride / 4u;
    uint32_t bytes;
    for (int32_t y = area->y1; y <= area->y2; y++) {
        uint32_t *dst = fb_px + (uint32_t)y * stride_px + (uint32_t)area->x1;
        memcpy(dst, px, (size_t)w * 4u);
        px += w * 4;
    }
    bytes = (uint32_t)w * (uint32_t)h * 4u;
    flush_n++;
    if (bytes > flush_max_bytes)
        flush_max_bytes = bytes;
    take_dt(&click_stamp, click_dt, &n_click, "click");
    take_dt(&key_stamp, key_dt, &n_key, "key");
    take_dt(&scroll_stamp, scroll_dt, &n_scroll, "scroll");
    lv_display_flush_ready(disp);
}

static uint32_t map_key(uint16_t code)
{
    static const char row1[] = "qwertyuiop";
    static const char row2[] = "asdfghjkl";
    static const char row3[] = "zxcvbnm";
    if (code >= 2 && code <= 10)
        return (uint32_t)('1' + (code - 2));
    if (code == 11)
        return '0';
    if (code >= 16 && code <= 25)
        return (uint32_t)row1[code - 16];
    if (code >= 30 && code <= 38)
        return (uint32_t)row2[code - 30];
    if (code >= 44 && code <= 50)
        return (uint32_t)row3[code - 44];
    if (code == 57)
        return ' ';
    if (code == 28)
        return LV_KEY_ENTER;
    if (code == 14)
        return LV_KEY_BACKSPACE;
    return 0;
}

static void compact(struct quota_input *q, int *n, int *i)
{
    int used = *i;
    if (!used)
        return;
    int left = *n - used;
    for (int k = 0; k < left; k++)
        q[k] = q[used + k];
    *n = left;
    *i = 0;
}

static void take_inputs(void)
{
    /* Keep events LVGL has not read yet. A refresh can be later than the virtio poll. */
    compact(keys, &nkeys, &key_i);
    compact(ptrs, &nptrs, &ptr_i);
    input_service_poll();
    struct quota_input ev;
    while (input_pop(&ev)) {
        if (ev.kind == QUOTA_INPUT_KEY && nkeys < 32)
            keys[nkeys++] = ev;
        else if (ev.kind == QUOTA_INPUT_POINTER && nptrs < 32)
            ptrs[nptrs++] = ev;
    }
}

static void pointer_read(lv_indev_t *indev, lv_indev_data_t *data)
{
    (void)indev;
    static int x, y, pressed;
    data->continue_reading = false;
    while (ptr_i < nptrs) {
        struct quota_input ev = ptrs[ptr_i++];
        x = ev.x;
        y = ev.y;
        pressed = ev.value != 0;
        if (pressed)
            click_stamp = now_us();
        printf("[lvgl] pointer %d,%d down %d\n", x, y, pressed);
        if (ptr_i < nptrs)
            data->continue_reading = true;
        break;
    }
    data->point.x = x;
    data->point.y = y;
    data->state = pressed ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED;
}

static void keypad_read(lv_indev_t *indev, lv_indev_data_t *data)
{
    (void)indev;
    static uint32_t last;
    data->key = last;
    data->state = LV_INDEV_STATE_RELEASED;
    data->continue_reading = false;
    while (key_i < nkeys) {
        struct quota_input ev = keys[key_i++];
        uint32_t mapped = map_key(ev.code);
        printf("[lvgl] key code %u value %u char %u\n", ev.code, ev.value, mapped);
        if (ev.value)
            key_stamp = now_us();
        if (!mapped)
            continue;
        last = mapped;
        data->key = mapped;
        data->state = ev.value ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED;
        if (key_i < nkeys)
            data->continue_reading = true;
        break;
    }
}

static void on_click(lv_event_t *e)
{
    (void)e;
    clicks++;
    lv_label_set_text_fmt(count_label, "Count: %u", clicks);
    printf("[lvgl] click count %u\n", clicks);
}

static void on_group(lv_event_t *e)
{
    int id = (int)(uintptr_t)lv_event_get_user_data(e);
    printf("[menu] button %d\n", id);
    if (id == 100) {
        scroll_stamp = now_us();
        if (menu_list)
            lv_obj_scroll_by(menu_list, 0, -36, LV_ANIM_OFF);
        printf("[lvgl] scroll once, one row, animation off\n");
        return;
    }
    if (id == 101) {
        printf("[lvgl] runaway not available: no protected process. An unbounded loop is not started.\n");
        return;
    }
    if (id == 102) {
        printf("[lvgl] ram crash not available. Menu 4 refuses an oversize quota. It does not exhaust RAM.\n");
        return;
    }
    if (id == 103) {
        demo_menu_stop();
        return;
    }
    if (id == 104) {
        printf("[lvgl] click and key samples are taken from real events. Panel light is not measured.\n");
        return;
    }
    if (id == 200) {
        show_page(1);
        printf("[lvgl] input page\n");
        return;
    }
    if (id == 201) {
        show_page(2);
        printf("[lvgl] coverage page\n");
        return;
    }
    if (id == 202) {
        show_page(0);
        printf("[lvgl] home\n");
        return;
    }
    demo_menu_start(id);
}

static lv_obj_t *add_row(lv_obj_t *parent, int y, uint32_t bg, uint32_t edge, const char *text, int id)
{
    lv_obj_t *item = lv_button_create(parent);
    int32_t cw = lv_obj_get_content_width(parent);
    if (cw < 40)
        cw = 230;
    lv_obj_set_pos(item, 0, y);
    lv_obj_set_size(item, cw, 32);
    lv_obj_set_style_bg_color(item, lv_color_hex(bg), 0);
    lv_obj_set_style_bg_opa(item, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(item, lv_color_hex(edge), 0);
    lv_obj_set_style_border_width(item, 2, 0);
    lv_obj_set_style_text_color(item, lv_color_hex(0x101820), 0);
    lv_obj_add_event_cb(item, on_group, LV_EVENT_CLICKED, (void *)(uintptr_t)id);
    lv_obj_t *il = lv_label_create(item);
    lv_label_set_text(il, text);
    lv_obj_set_style_text_color(il, lv_color_hex(0x101820), 0);
    lv_obj_center(il);
    return item;
}

static lv_obj_t *add_head(lv_obj_t *parent, int y, uint32_t bg, const char *text)
{
    lv_obj_t *lab = lv_label_create(parent);
    lv_obj_set_pos(lab, 0, y);
    lv_obj_set_style_bg_color(lab, lv_color_hex(bg), 0);
    lv_obj_set_style_bg_opa(lab, LV_OPA_COVER, 0);
    lv_obj_set_style_text_color(lab, lv_color_hex(0x101820), 0);
    lv_label_set_text(lab, text);
    return lab;
}

int lvgl_demo_run(uint32_t *pixels, uint32_t w, uint32_t h, uint32_t stride)
{
    fb_px = pixels;
    fb_w = w;
    fb_h = h;
    fb_stride = stride;
    printf("[lvgl] upstream %d.%d.%d heap %u draw %u\n",
           LVGL_VERSION_MAJOR, LVGL_VERSION_MINOR, LVGL_VERSION_PATCH,
           (unsigned)LV_MEM_SIZE, (unsigned)sizeof draw_buf);

    lv_init();
    lv_tick_set_cb(lvgl_tick_ms);

    lv_display_t *disp = lv_display_create((int32_t)w, (int32_t)h);
    lv_display_set_flush_cb(disp, flush_cb);
    lv_display_set_buffers(disp, draw_buf, NULL, sizeof draw_buf, LV_DISPLAY_RENDER_MODE_PARTIAL);

    lv_indev_t *pointer = lv_indev_create();
    lv_indev_set_type(pointer, LV_INDEV_TYPE_POINTER);
    lv_indev_set_read_cb(pointer, pointer_read);
    lv_indev_t *keypad = lv_indev_create();
    lv_indev_set_type(keypad, LV_INDEV_TYPE_KEYPAD);
    lv_indev_set_read_cb(keypad, keypad_read);

    lv_obj_t *screen = lv_screen_active();
    lv_obj_set_style_bg_color(screen, lv_color_hex(0xffffff), 0);
    lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, 0);
    lv_obj_set_style_text_color(screen, lv_color_hex(0x101820), 0);
    build_cursor();
    lv_obj_t *cursor = lv_image_create(screen);
    lv_image_set_src(cursor, &cursor_dsc);
    lv_indev_set_cursor(pointer, cursor);
    printf("[lvgl] white background, dark text\n");
    printf("[lvgl] software cursor %dx%d black fill white outline hotspot 0,0\n", CUR_W, CUR_H);

    lv_obj_t *title = lv_label_create(screen);
    lv_label_set_text(title, "Quota OS menu");
    lv_obj_set_pos(title, 8, 2);
    lv_obj_set_style_text_color(title, lv_color_hex(0x101820), 0);

    legend = lv_label_create(screen);
    lv_obj_set_pos(legend, 8, 20);
    lv_obj_set_width(legend, 620);
    lv_obj_set_style_text_color(legend, lv_color_hex(0x101820), 0);
    {
        char leg[160];
        demo_menu_legend(leg, (int)sizeof leg);
        lv_label_set_text(legend, leg);
    }

    page_home = lv_obj_create(screen);
    lv_obj_set_pos(page_home, 0, 36);
    lv_obj_set_size(page_home, 640, 444);
    lv_obj_set_style_bg_opa(page_home, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(page_home, 0, 0);
    lv_obj_remove_flag(page_home, LV_OBJ_FLAG_SCROLLABLE);

    menu_list = lv_obj_create(page_home);
    lv_obj_set_pos(menu_list, 4, 0);
    lv_obj_set_size(menu_list, 250, 400);
    lv_obj_set_style_bg_color(menu_list, lv_color_hex(0xffffff), 0);
    lv_obj_set_style_bg_opa(menu_list, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(menu_list, lv_color_hex(0x101820), 0);
    lv_obj_set_style_border_width(menu_list, 1, 0);
    lv_obj_set_style_pad_all(menu_list, 2, 0);
    lv_obj_remove_flag(menu_list, LV_OBJ_FLAG_SCROLLABLE);
    /* Blue draw, green stress, orange admission, red fault. Text states stay on the row. */
    add_head(menu_list, 0, 0xdbeafe, "Draw");
    add_row(menu_list, 18, 0xdbeafe, 0x1d4ed8, "scroll once", 100);
    lv_obj_t *inb = add_row(menu_list, 52, 0xdbeafe, 0x1d4ed8, "input page", 200);
    add_head(menu_list, 90, 0xdcfce7, "Stress");
    add_row(menu_list, 108, 0xdcfce7, 0x166534, "tier-1 window", 5);
    lv_obj_t *load9 = add_row(menu_list, 142, 0xdcfce7, 0x166534, "tier-3 load", 9);
    add_row(menu_list, 176, 0xdcfce7, 0x166534, "stop", 103);
    add_head(menu_list, 214, 0xffedd5, "Admit");
    add_row(menu_list, 232, 0xffedd5, 0xc2410c, "install Pixel Racer", 2);
    add_row(menu_list, 266, 0xffedd5, 0xc2410c, "admit until full", 3);
    add_row(menu_list, 300, 0xffedd5, 0xc2410c, "reject too big", 4);
    add_head(menu_list, 338, 0xfee2e2, "Fault");
    lv_obj_t *covb = add_row(menu_list, 356, 0xfee2e2, 0xb91c1c, "coverage", 201);

    detail = lv_label_create(page_home);
    lv_obj_set_pos(detail, 258, 0);
    lv_obj_set_size(detail, 370, 430);
    lv_label_set_long_mode(detail, LV_LABEL_LONG_WRAP);
    lv_label_set_text(detail, "no result yet\nT1 miss not measured\np99 not measured\npages not measured");
    lv_obj_set_style_text_color(detail, lv_color_hex(0x101820), 0);
    lv_obj_set_style_bg_color(detail, lv_color_hex(0xf8fafc), 0);
    lv_obj_set_style_bg_opa(detail, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(detail, lv_color_hex(0x101820), 0);
    lv_obj_set_style_border_width(detail, 1, 0);
    lv_obj_set_style_pad_all(detail, 6, 0);

    page_input = lv_obj_create(screen);
    lv_obj_set_pos(page_input, 0, 36);
    lv_obj_set_size(page_input, 640, 444);
    lv_obj_set_style_bg_color(page_input, lv_color_hex(0xffffff), 0);
    lv_obj_set_style_bg_opa(page_input, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(page_input, 0, 0);
    lv_obj_remove_flag(page_input, LV_OBJ_FLAG_SCROLLABLE);
    add_row(page_input, 8, 0xffffff, 0x101820, "back", 202);
    lv_obj_t *btn = lv_button_create(page_input);
    lv_obj_set_pos(btn, 8, 52);
    lv_obj_set_size(btn, 140, 40);
    lv_obj_set_style_bg_color(btn, lv_color_hex(0xdbeafe), 0);
    lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(btn, lv_color_hex(0x1d4ed8), 0);
    lv_obj_set_style_border_width(btn, 2, 0);
    lv_obj_add_event_cb(btn, on_click, LV_EVENT_CLICKED, NULL);
    lv_obj_t *bl = lv_label_create(btn);
    lv_label_set_text(bl, "+1");
    lv_obj_set_style_text_color(bl, lv_color_hex(0x101820), 0);
    lv_obj_center(bl);
    count_label = lv_label_create(page_input);
    lv_label_set_text(count_label, "Count: 0");
    lv_obj_set_pos(count_label, 160, 60);
    lv_obj_set_style_text_color(count_label, lv_color_hex(0x101820), 0);
    field = lv_textarea_create(page_input);
    lv_obj_set_pos(field, 8, 108);
    lv_obj_set_size(field, 400, 40);
    lv_textarea_set_one_line(field, true);
    lv_textarea_set_placeholder_text(field, "type here");
    lv_obj_set_style_bg_color(field, lv_color_hex(0xffffff), 0);
    lv_obj_set_style_bg_opa(field, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(field, lv_color_hex(0x101820), 0);
    lv_obj_set_style_border_width(field, 2, 0);
    lv_obj_set_style_text_color(field, lv_color_hex(0x101820), 0);
    {
        lv_group_t *group = lv_group_create();
        lv_group_add_obj(group, field);
        lv_indev_set_group(keypad, group);
    }
    lv_obj_add_flag(page_input, LV_OBJ_FLAG_HIDDEN);

    page_cover = lv_obj_create(screen);
    lv_obj_set_pos(page_cover, 0, 36);
    lv_obj_set_size(page_cover, 640, 444);
    lv_obj_set_style_bg_color(page_cover, lv_color_hex(0xffffff), 0);
    lv_obj_set_style_bg_opa(page_cover, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(page_cover, 0, 0);
    lv_obj_remove_flag(page_cover, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t *back = add_row(page_cover, 4, 0xffffff, 0x101820, "back", 202);
    {
        lv_obj_t *cov = lv_label_create(page_cover);
        lv_obj_set_pos(cov, 8, 44);
        lv_obj_set_width(cov, 620);
        lv_obj_set_style_text_color(cov, lv_color_hex(0x101820), 0);
        lv_label_set_text(cov,
            "Not connected. Serial menu still runs them.\n"
            "6 tier-1 slot cut: spin task in a tier-1 slot\n"
            "10 runaway: same, unbounded loop is not started\n"
            "13 budget token: sched_bench blocks the shell\n"
            "14 device timeout: sched_bench blocks the shell\n"
            "15 interrupt storm: sched_bench blocks the shell\n"
            "16 bandwidth cap: sched_bench blocks the shell\n"
            "18 IPC hold: four sched_bench windows block the shell\n"
            "Unbounded runaway and a RAM crash are not started.");
    }
    lv_obj_add_flag(page_cover, LV_OBJ_FLAG_HIDDEN);

    lv_obj_update_layout(screen);
    {
        lv_area_t box;
        lv_obj_get_coords(load9, &box);
        printf("[lvgl] load9 box %d,%d %dx%d\n", (int)box.x1, (int)box.y1,
               (int)(box.x2 - box.x1 + 1), (int)(box.y2 - box.y1 + 1));
        lv_obj_get_coords(inb, &box);
        printf("[lvgl] input box %d,%d %dx%d\n", (int)box.x1, (int)box.y1,
               (int)(box.x2 - box.x1 + 1), (int)(box.y2 - box.y1 + 1));
        lv_obj_get_coords(covb, &box);
        printf("[lvgl] cover box %d,%d %dx%d\n", (int)box.x1, (int)box.y1,
               (int)(box.x2 - box.x1 + 1), (int)(box.y2 - box.y1 + 1));
        lv_obj_get_coords(back, &box);
        printf("[lvgl] back box %d,%d %dx%d\n", (int)box.x1, (int)box.y1,
               (int)(box.x2 - box.x1 + 1), (int)(box.y2 - box.y1 + 1));
        lv_obj_get_coords(btn, &box);
        printf("[lvgl] plus box %d,%d %dx%d\n", (int)box.x1, (int)box.y1,
               (int)(box.x2 - box.x1 + 1), (int)(box.y2 - box.y1 + 1));
    }
    printf("[menu] serial 6 10 13 14 15 16 18 stay on the coverage page\n");
    printf("[lvgl] ready\n");

    uint64_t t0 = now_us();
    /* The shell stack is 8 KB. These stay off that stack. */
    static char shown[480];
    static char leg_shown[160];
    static char text[480];
    static struct menu_detail d;
    shown[0] = 0;
    leg_shown[0] = 0;
    while (now_us() - t0 < 45000000ull) {
        int i;
        uint64_t cmax = 0, kmax = 0, smax = 0;
        take_inputs();
        lv_timer_handler();
        demo_menu_detail(&d);
        for (i = 0; i < n_click; i++)
            if (click_dt[i] > cmax)
                cmax = click_dt[i];
        for (i = 0; i < n_key; i++)
            if (key_dt[i] > kmax)
                kmax = key_dt[i];
        for (i = 0; i < n_scroll; i++)
            if (scroll_dt[i] > smax)
                smax = scroll_dt[i];
        if (d.running) {
            lv_snprintf(text, sizeof text,
                        "menu %d running %u/%u\n"
                        "T1 miss not measured yet\n"
                        "class a counts when the window ends\n"
                        "p99 not measured yet\n"
                        "pages not measured\n"
                        "panel light not measured",
                        d.id, d.progress, d.total);
        } else if (d.have_window) {
            char p99line[72];
            if (d.p99_ok)
                lv_snprintf(p99line, sizeof p99line, "%lu us nearest-rank ceil(0.99*n)", d.p99_us);
            else
                lv_snprintf(p99line, sizeof p99line, "not measured n<20 or capped");
            lv_snprintf(text, sizeof text,
                        "menu %d done %u/%u\n"
                        "T1 miss %u\n"
                        "class a %lu  b %lu  skipped %lu\n"
                        "class a counts, not a pass\n"
                        "%s\n"
                        "done max %lu us n=%u%s\n"
                        "p99 %s\n"
                        "not_started %u unfinished %u\n"
                        "open bound %lu us not a completion\n"
                        "shell %u us/period\n"
                        "quota T1 1500us 13312KB\n"
                        "pages not measured\n"
                        "panel light not measured",
                        d.id, d.progress, d.total,
                        d.t1_miss, d.class_a, d.class_b, d.skipped,
                        clock_is_icount() ? "icount" : "host clock, not a pass",
                        d.lat_max_us, d.samples, d.capped ? " capped" : "",
                        p99line,
                        d.not_started, d.unfinished, d.open_bound_us,
                        d.shell_us);
            if (d.t2_ok) {
                char extra[80];
                int used = 0;
                int k = 0;
                lv_snprintf(extra, sizeof extra, "\ntier2 short %u  job miss %u", d.t2_short, d.job_miss);
                while (text[used])
                    used++;
                while (extra[k] && used + 1 < (int)sizeof text)
                    text[used++] = extra[k++];
                text[used] = 0;
            }
        } else {
            lv_snprintf(text, sizeof text,
                        "no window yet\n"
                        "T1 miss not measured\n"
                        "p99 not measured\n"
                        "pages not measured\n"
                        "panel light not measured\n"
                        "fb click n=%u max %lu us\n"
                        "fb key n=%u max %lu us\n"
                        "fb scroll n=%u max %lu us",
                        (unsigned)n_click, cmax, (unsigned)n_key, kmax, (unsigned)n_scroll, smax);
        }
        if (strcmp(text, shown) != 0) {
            lv_label_set_text(detail, text);
            int n = 0;
            while (text[n] && n < (int)sizeof shown - 1) {
                shown[n] = text[n];
                n++;
            }
            shown[n] = 0;
            if (!d.running && d.have_window)
                printf("[lvgl] detail\n%s\n", text);
        }
        {
            char leg[160];
            demo_menu_legend(leg, (int)sizeof leg);
            if (strcmp(leg, leg_shown) != 0) {
                lv_label_set_text(legend, leg);
                printf("[lvgl] legend %s\n", leg);
                int n = 0;
                while (leg[n] && n < (int)sizeof leg_shown - 1) {
                    leg_shown[n] = leg[n];
                    n++;
                }
                leg_shown[n] = 0;
            }
        }
    }
    printf("[lvgl] result clicks %u drops %u\n", clicks, input_drop_count());
    return 0;
}
