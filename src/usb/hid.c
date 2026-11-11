/* HID report formats: find the HID interfaces of a device, read a report
 * descriptor, and take an input report apart. Nothing here touches hardware,
 * so the same file is compiled and checked on the build machine.
 *
 * A report descriptor is a list of short items. Global items (usage page,
 * report size and count, report ID) stay in force until changed; local items
 * (the usages) apply to the next main item only; an Input main item then
 * declares `count` fields of `size` bits. Fields are laid out in order, per
 * report ID, which is how the bit offsets below are found. */
#ifdef __x86_64__
#include "hid.h"

#define MAX_USAGES 16
#define MAX_IDS    16
#define MAX_STACK  4

enum { APP_NONE = 0, APP_KEYBOARD, APP_MOUSE };

int hid_find_interfaces(const uint8_t *cfg, int len, hid_iface_t *out, int max)
{
    int n = 0, i = 0;
    bool open = false;                      /* inside a HID interface, alternate setting 0 */
    hid_iface_t cur = { 0, 0, 0, 0, 0, 0, 0 };
    while (i + 2 <= len && cfg[i] >= 2 && i + cfg[i] <= len) {
        const uint8_t *p = cfg + i;
        if (p[1] == 4 && p[0] >= 9) {
            if (open && cur.ep && n < max) out[n++] = cur;
            open = p[5] == 3 && p[3] == 0;
            cur = (hid_iface_t){ p[2], p[6], p[7], 0, 0, 0, 0 };
        } else if (open && p[1] == 0x21 && p[0] >= 9) {
            if (p[6] == 0x22) cur.rlen = (uint16_t)(p[7] | (p[8] << 8));
        } else if (open && p[1] == 5 && p[0] >= 7) {
            if ((p[2] & 0x80) && (p[3] & 3) == 3 && !cur.ep) {
                cur.ep = p[2] & 0x0F;
                cur.maxpkt = (uint16_t)((p[4] | (p[5] << 8)) & 0x7FF);
                cur.interval = p[6];
            }
        }
        i += p[0];
    }
    if (open && cur.ep && n < max) out[n++] = cur;
    return n;
}

int hid_field(const uint8_t *p, int n, int off, int bits, bool sign)
{
    if (bits <= 0 || bits > 32 || off < 0) return 0;
    uint32_t v = 0;
    for (int i = 0; i < bits; i++) {
        int b = off + i;
        if ((b >> 3) >= n) break;
        v |= (uint32_t)((p[b >> 3] >> (b & 7)) & 1) << i;
    }
    if (sign && bits < 32 && (v & (1u << (bits - 1)))) v |= ~((1u << bits) - 1u);
    return (int)v;
}

typedef struct { uint32_t page; int rsize, rcount, id; } globals_t;

typedef struct {
    globals_t g, stack[MAX_STACK];
    int depth_stack;
    uint32_t usage[MAX_USAGES];
    bool wide[MAX_USAGES];                  /* the usage item carried its own page */
    int nusage;
    uint32_t umin, umax;
    bool umin_wide, umax_wide, range;
    int ids[MAX_IDS], bits[MAX_IDS], nids;
} parse_t;

/* A short usage belongs to the page in force at the main item, not at the
 * usage item, which is how the HID parsers in real systems read it. */
static uint32_t full_usage(const parse_t *s, uint32_t u, bool wide)
{
    return wide ? u : (s->g.page << 16) | (u & 0xFFFF);
}
static uint32_t field_usage(const parse_t *s, int f)
{
    if (s->nusage) {
        int k = f < s->nusage ? f : s->nusage - 1;
        return full_usage(s, s->usage[k], s->wide[k]);
    }
    if (s->range) {
        uint32_t lo = full_usage(s, s->umin, s->umin_wide), hi = full_usage(s, s->umax, s->umax_wide);
        uint32_t u = lo + (uint32_t)f;
        return u > hi ? hi : u;
    }
    return 0;
}
static void clear_locals(parse_t *s) { s->nusage = 0; s->range = false; s->umin = s->umax = 0; }

/* where the next field of this report ID starts */
static int *bit_pos(parse_t *s, int id)
{
    static int spare;
    for (int i = 0; i < s->nids; i++) if (s->ids[i] == id) return &s->bits[i];
    if (s->nids >= MAX_IDS) { spare = 0; return &spare; }
    s->ids[s->nids] = id; s->bits[s->nids] = 0;
    return &s->bits[s->nids++];
}

void hid_parse_format(hid_format_t *f, const uint8_t *p, int n)
{
    static parse_t st;
    parse_t *s = &st;
    *s = (parse_t){ 0 };
    *f = (hid_format_t){ 0 };
    f->keys.mod_off = -1;

    int depth = 0, app = APP_NONE;
    int x_id = -1, y_id = -1, btn_id = -1, w_id = -1, key_id = -1;
    bool absolute = false;

    int i = 0;
    while (i < n) {
        uint8_t b = p[i];
        if (b == 0xFE) {                                  /* long item: skip it */
            if (i + 3 > n) break;
            i += 3 + p[i + 1];
            continue;
        }
        int sz = (b & 3) == 3 ? 4 : (b & 3);
        if (i + 1 + sz > n) break;
        uint32_t val = 0;
        for (int k = 0; k < sz; k++) val |= (uint32_t)p[i + 1 + k] << (8 * k);
        i += 1 + sz;

        switch (b & 0xFC) {
        /* global items */
        case 0x04: s->g.page = val & 0xFFFF; break;
        case 0x74: s->g.rsize = (int)val; break;
        case 0x94: s->g.rcount = (int)val; break;
        case 0x84: s->g.id = (int)val; f->ids = true; break;
        case 0xA4: if (s->depth_stack < MAX_STACK) s->stack[s->depth_stack++] = s->g; break;
        case 0xB4: if (s->depth_stack > 0) s->g = s->stack[--s->depth_stack]; break;
        /* local items */
        case 0x08:
            if (s->nusage < MAX_USAGES) { s->usage[s->nusage] = val; s->wide[s->nusage] = sz == 4; s->nusage++; }
            break;
        case 0x18: s->umin = val; s->umin_wide = sz == 4; s->range = true; break;
        case 0x28: s->umax = val; s->umax_wide = sz == 4; s->range = true; break;
        /* main items */
        case 0xA0:                                        /* Collection */
            if (depth == 0 && val == 1) {                 /* a top-level application */
                uint32_t u = s->nusage ? full_usage(s, s->usage[s->nusage - 1], s->wide[s->nusage - 1]) : 0;
                app = (u == 0x10006 || u == 0x10007) ? APP_KEYBOARD
                    : (u == 0x10002 || u == 0x10001) ? APP_MOUSE : APP_NONE;
            }
            depth++;
            clear_locals(s);
            break;
        case 0xC0:                                        /* End Collection */
            if (depth > 0) depth--;
            if (depth == 0) app = APP_NONE;
            clear_locals(s);
            break;
        case 0x80: {                                      /* Input */
            int *pos = bit_pos(s, s->g.id);
            int size = s->g.rsize, count = s->g.rcount, id = s->g.id;
            bool constant = val & 1, variable = val & 2, relative = val & 4;
            if (size <= 0 || count <= 0 || size > 32 || count > 4096) { clear_locals(s); break; }
            if (!constant && app == APP_MOUSE) {
                for (int k = 0; k < count && k < 64; k++) {
                    uint32_t u = field_usage(s, k);
                    int at = *pos + k * size;
                    if (u == 0x10030 || u == 0x10031) {
                        if (!relative) absolute = true;
                        else if (u == 0x10030 && !f->mouse.x_bits) { f->mouse.x_off = at; f->mouse.x_bits = size; x_id = id; }
                        else if (u == 0x10031 && !f->mouse.y_bits) { f->mouse.y_off = at; f->mouse.y_bits = size; y_id = id; }
                    } else if (u == 0x10038 && relative && !f->mouse.w_bits) {
                        f->mouse.w_off = at; f->mouse.w_bits = size; w_id = id;
                    } else if ((u >> 16) == 0x09 && variable && size == 1) {
                        if (!f->mouse.btn_bits) { f->mouse.btn_off = at; f->mouse.btn_bits = 1; btn_id = id; }
                        else if (id == btn_id && at == f->mouse.btn_off + f->mouse.btn_bits) f->mouse.btn_bits++;
                    }
                }
            } else if (!constant && app == APP_KEYBOARD && (field_usage(s, 0) >> 16) == 0x07 &&
                       (key_id < 0 || key_id == id)) {
                uint32_t first = field_usage(s, 0) & 0xFFFF;
                if (variable && size == 1 && first == 0xE0 && count == 8 && f->keys.mod_off < 0) {
                    f->keys.mod_off = *pos; key_id = id;
                } else if (variable && size == 1 && s->range && !s->nusage && !f->keys.map_count) {
                    int span = (int)((s->umax & 0xFFFF) - (s->umin & 0xFFFF)) + 1;
                    f->keys.map_off = *pos; f->keys.map_min = (int)first;
                    f->keys.map_count = count < span ? count : span;
                    key_id = id;
                } else if (!variable && !f->keys.key_count) {
                    f->keys.key_off = *pos; f->keys.key_bits = size;
                    f->keys.key_count = count > 32 ? 32 : count;
                    key_id = id;
                }
            }
            *pos += size * count;
            clear_locals(s);
            break;
        }
        case 0x90: case 0xB0:                             /* Output, Feature */
            clear_locals(s);
            break;
        }
    }

    f->mouse.present = f->mouse.x_bits && f->mouse.y_bits && x_id == y_id && !absolute;
    f->mouse.id = x_id < 0 ? 0 : x_id;
    if (btn_id != x_id) f->mouse.btn_bits = 0;
    if (w_id != x_id) f->mouse.w_bits = 0;
    f->keys.present = f->keys.key_count > 0 || f->keys.map_count > 0;
    f->keys.id = key_id < 0 ? 0 : key_id;
    f->mouse.len = (*bit_pos(s, f->mouse.id) + 7) / 8;
    f->keys.len = (*bit_pos(s, f->keys.id) + 7) / 8;
}

bool hid_decode_mouse(const hid_format_t *f, const uint8_t *p, int n,
                      int *dx, int *dy, int *buttons, int *wheel)
{
    const int need_x = f->mouse.x_off + f->mouse.x_bits, need_y = f->mouse.y_off + f->mouse.y_bits;
    if (!f->mouse.present || n * 8 < need_x || n * 8 < need_y) return false;
    *dx = hid_field(p, n, f->mouse.x_off, f->mouse.x_bits, true);
    *dy = hid_field(p, n, f->mouse.y_off, f->mouse.y_bits, true);
    *buttons = f->mouse.btn_bits ? hid_field(p, n, f->mouse.btn_off, f->mouse.btn_bits, false) : 0;
    *wheel = f->mouse.w_bits ? hid_field(p, n, f->mouse.w_off, f->mouse.w_bits, true) : 0;
    return true;
}

static void add_key(uint8_t out[8], int *nkeys, int u)
{
    if (u >= 0xE0 && u <= 0xE7) { out[0] |= (uint8_t)(1u << (u - 0xE0)); return; }
    if (u < 4 || u > 0xFF) return;                        /* 1..3 are error codes */
    for (int i = 0; i < *nkeys; i++) if (out[2 + i] == u) return;
    if (*nkeys < 6) out[2 + (*nkeys)++] = (uint8_t)u;
}

bool hid_decode_keys(const hid_format_t *f, const uint8_t *p, int n, uint8_t out[8])
{
    for (int i = 0; i < 8; i++) out[i] = 0;
    if (!f->keys.present) return false;
    int nkeys = 0;
    if (f->keys.mod_off >= 0) out[0] = (uint8_t)hid_field(p, n, f->keys.mod_off, 8, false);
    for (int i = 0; i < f->keys.key_count; i++) {
        int u = hid_field(p, n, f->keys.key_off + i * f->keys.key_bits, f->keys.key_bits, false);
        if (u == 1) return false;                         /* ErrorRollOver: too many keys */
        add_key(out, &nkeys, u);
    }
    for (int i = 0; i < f->keys.map_count; i++)
        if (hid_field(p, n, f->keys.map_off + i, 1, false)) add_key(out, &nkeys, f->keys.map_min + i);
    return true;
}
#endif
