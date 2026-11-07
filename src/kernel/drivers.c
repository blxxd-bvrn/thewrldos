/* PIT timer, CMOS RTC, PS/2 keyboard & mouse, PC speaker */
#include "kernel.h"

/* ---------------- timer ---------------- */
static volatile uint32_t ticks;
volatile uint32_t idle_ticks;
volatile bool cpu_idle_flag;
void (*timer_tick_hook)(uint32_t ticks);    /* runs in interrupt context every tick */
void (*watchdog_hook)(regs_t *r, uint32_t ticks);
static void timer_irq(regs_t *r) {
    ticks++;
    if (timer_tick_hook) timer_tick_hook(ticks);
    if (watchdog_hook && !(ticks & 63)) watchdog_hook(r, ticks);
    if (!(ticks & 15)) { extern void ps2_poll(void); ps2_poll(); }
    if (cpu_idle_flag) idle_ticks++;
    else { task_t *t = task_current(); if (t) t->cpu_ms++; }
}
void timer_init(void) {
    uint32_t div = 1193182 / TIMER_HZ;
    outb(0x43, 0x36);
    outb(0x40, div & 0xFF);
    outb(0x40, div >> 8);
    irq_install(0, timer_irq);
}
uint32_t timer_ticks(void) { return ticks; }
void sleep_ms(uint32_t ms) {
    uint32_t end = ticks + ms;
    while ((int32_t)(end - ticks) > 0) hlt();
}

/* ---------------- speaker ---------------- */
void speaker_beep(uint32_t freq, uint32_t ms) {
    if (freq) {
        uint32_t div = 1193180 / freq;
        outb(0x43, 0xB6); outb(0x42, div & 0xFF); outb(0x42, div >> 8);
        outb(0x61, inb(0x61) | 3);
    }
    task_sleep(ms);
    outb(0x61, inb(0x61) & 0xFC);
}

/* ---------------- RTC ---------------- */
int tz_offset_minutes = 0;
static uint8_t cmos(uint8_t r) { outb(0x70, r); return inb(0x71); }
static int bcd(int v) { return (v & 0x0F) + ((v >> 4) * 10); }
void rtc_read(rtc_time_t *t) {
    while (cmos(0x0A) & 0x80) {}
    int s = cmos(0), m = cmos(2), h = cmos(4), d = cmos(7), mo = cmos(8), y = cmos(9);
    int b = cmos(0x0B);
    if (!(b & 4)) { s = bcd(s); m = bcd(m); h = ((h & 0x7F) ? bcd(h & 0x7F) : 0) | (h & 0x80); d = bcd(d); mo = bcd(mo); y = bcd(y); }
    if (!(b & 2) && (h & 0x80)) h = ((h & 0x7F) + 12) % 24;
    t->second = s; t->minute = m; t->hour = h & 0x7F; t->day = d; t->month = mo; t->year = 2000 + y;
    /* Zeller-like weekday (0 = Sunday) */
    int yy = t->year, mm = t->month;
    if (mm < 3) { mm += 12; yy--; }
    t->weekday = (t->day + 13 * (mm + 1) / 5 + yy + yy / 4 - yy / 100 + yy / 400 + 6) % 7;
}
static int days_in_month(int y, int m) {
    static const int d[] = { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };
    if (m == 2 && ((y % 4 == 0 && y % 100) || y % 400 == 0)) return 29;
    return d[m - 1];
}
void rtc_now(rtc_time_t *t) {
    rtc_read(t);
    int mins = t->hour * 60 + t->minute + tz_offset_minutes;
    while (mins < 0) { mins += 1440; t->weekday = (t->weekday + 6) % 7; if (--t->day < 1) { if (--t->month < 1) { t->month = 12; t->year--; } t->day = days_in_month(t->year, t->month); } }
    while (mins >= 1440) { mins -= 1440; t->weekday = (t->weekday + 1) % 7; if (++t->day > days_in_month(t->year, t->month)) { t->day = 1; if (++t->month > 12) { t->month = 1; t->year++; } } }
    t->hour = mins / 60; t->minute = mins % 60;
}

/* ---------------- keyboard ---------------- */
#define KQ 128
static key_event_t kq[KQ];
static volatile uint32_t kq_head, kq_tail;
static int kb_mods;
static bool kb_caps, kb_e0;
static const char sc_lower[128] = {
    0, 27, '1','2','3','4','5','6','7','8','9','0','-','=', 8, 9,
    'q','w','e','r','t','y','u','i','o','p','[',']', 10, 0, 'a','s',
    'd','f','g','h','j','k','l',';','\'','`', 0, '\\','z','x','c','v',
    'b','n','m',',','.','/', 0, '*', 0, ' ', 0 };
static const char sc_upper[128] = {
    0, 27, '!','@','#','$','%','^','&','*','(',')','_','+', 8, 9,
    'Q','W','E','R','T','Y','U','I','O','P','{','}', 10, 0, 'A','S',
    'D','F','G','H','J','K','L',':','"','~', 0, '|','Z','X','C','V',
    'B','N','M','<','>','?', 0, '*', 0, ' ', 0 };

static void kq_push(key_event_t e) {
    uint32_t n = (kq_head + 1) % KQ;
    if (n == kq_tail) return;
    kq[kq_head] = e; kq_head = n;
}
static void kb_byte(uint8_t sc) {
    if (sc == 0xE0) { kb_e0 = true; return; }
    bool rel = sc & 0x80;
    sc &= 0x7F;
    key_event_t e = { 0, 0, !rel, 0 };
    if (kb_e0) {
        kb_e0 = false;
        switch (sc) {
        case 0x48: e.key = KEY_UP; break; case 0x50: e.key = KEY_DOWN; break;
        case 0x4B: e.key = KEY_LEFT; break; case 0x4D: e.key = KEY_RIGHT; break;
        case 0x47: e.key = KEY_HOME; break; case 0x4F: e.key = KEY_END; break;
        case 0x49: e.key = KEY_PGUP; break; case 0x51: e.key = KEY_PGDN; break;
        case 0x52: e.key = KEY_INSERT; break; case 0x53: e.key = KEY_DELETE; break;
        case 0x1C: e.key = KEY_ENTER; e.ch = '\n'; break;
        case 0x1D: e.key = KEY_LCTRL; if (rel) kb_mods &= ~MOD_CTRL; else kb_mods |= MOD_CTRL; break;
        case 0x38: e.key = KEY_LALT; if (rel) kb_mods &= ~MOD_ALT; else kb_mods |= MOD_ALT; break;
        case 0x5B: case 0x5C: e.key = KEY_WIN; break;
        case 0x5D: e.key = KEY_MENU; break;
        case 0x35: e.key = '/'; e.ch = '/'; break;
        default: return;
        }
        e.mods = kb_mods; kq_push(e); return;
    }
    switch (sc) {
    case 0x2A: case 0x36: if (rel) kb_mods &= ~MOD_SHIFT; else kb_mods |= MOD_SHIFT; e.key = sc == 0x2A ? KEY_LSHIFT : KEY_RSHIFT; break;
    case 0x1D: if (rel) kb_mods &= ~MOD_CTRL; else kb_mods |= MOD_CTRL; e.key = KEY_LCTRL; break;
    case 0x38: if (rel) kb_mods &= ~MOD_ALT; else kb_mods |= MOD_ALT; e.key = KEY_LALT; break;
    case 0x3A: if (!rel) kb_caps = !kb_caps; e.key = KEY_CAPS; break;
    case 0x01: e.key = KEY_ESC; e.ch = 27; break;
    case 0x0E: e.key = KEY_BACKSPACE; e.ch = 8; break;
    case 0x0F: e.key = KEY_TAB; e.ch = 9; break;
    case 0x1C: e.key = KEY_ENTER; e.ch = '\n'; break;
    case 0x3B: case 0x3C: case 0x3D: case 0x3E: case 0x3F: case 0x40: case 0x41: case 0x42: case 0x43: case 0x44:
        e.key = KEY_F1 + (sc - 0x3B); break;
    case 0x57: e.key = KEY_F11; break;
    case 0x58: e.key = KEY_F12; break;
    case 0x48: e.key = KEY_UP; break; case 0x50: e.key = KEY_DOWN; break;
    case 0x4B: e.key = KEY_LEFT; break; case 0x4D: e.key = KEY_RIGHT; break;
    case 0x47: e.key = KEY_HOME; break; case 0x4F: e.key = KEY_END; break;
    case 0x49: e.key = KEY_PGUP; break; case 0x51: e.key = KEY_PGDN; break;
    case 0x53: e.key = KEY_DELETE; break;
    default: {
        bool shift = kb_mods & MOD_SHIFT;
        char c = shift ? sc_upper[sc] : sc_lower[sc];
        if (kb_caps && isalpha(c)) c = shift ? tolower(c) : toupper(c);
        if (!c) return;
        e.key = c; e.ch = c;
    }
    }
    e.mods = kb_mods;
    kq_push(e);
}
static void ps2_service(void);
static void kb_irq(regs_t *r) { (void)r; ps2_service(); }
void keyboard_init(void) {
    while (inb(0x64) & 1) inb(0x60);
    irq_install(1, kb_irq);
}
bool keyboard_poll(key_event_t *ev) {
    if (kq_tail == kq_head) return false;
    *ev = kq[kq_tail];
    kq_tail = (kq_tail + 1) % KQ;
    return true;
}

/* ---------------- mouse ---------------- */
int mouse_speed = 5;
bool mouse_swap_buttons = false;
static volatile int ms_x, ms_y, ms_buttons, ms_wheel;
static volatile bool ms_changed;
static int ms_w, ms_h, ms_cycle, ms_pkt_size = 3;
static uint8_t ms_pkt[4];

static void ps2_wait_write(void) { for (int i = 0; i < 100000 && (inb(0x64) & 2); i++) {} }
static void ps2_wait_read(void) { for (int i = 0; i < 100000 && !(inb(0x64) & 1); i++) {} }
static void mouse_write(uint8_t v) { ps2_wait_write(); outb(0x64, 0xD4); ps2_wait_write(); outb(0x60, v); }
static uint8_t mouse_read(void) { ps2_wait_read(); return inb(0x60); }

static volatile bool ps2_ready;              /* both devices initialised */
static uint32_t ms_last;
static void mouse_byte(uint8_t b) {
    if (ms_cycle && ticks - ms_last > 40) ms_cycle = 0;   /* a packet's bytes arrive together: drop a stale half */
    ms_last = ticks;
    if (ms_cycle == 0 && !(b & 0x08)) return;   /* resync */
    ms_pkt[ms_cycle++] = b;
    if (ms_cycle < ms_pkt_size) return;
    ms_cycle = 0;
    if (ms_pkt[0] & 0xC0) return;               /* overflow */
    int dx = ms_pkt[1] - ((ms_pkt[0] & 0x10) ? 256 : 0);
    int dy = ms_pkt[2] - ((ms_pkt[0] & 0x20) ? 256 : 0);
    int sp = mouse_speed;
    dx = dx * sp / 5; dy = dy * sp / 5;
    int ad = (dx < 0 ? -dx : dx) + (dy < 0 ? -dy : dy);
    if (sp > 5 && ad > 6) { dx = dx * 3 / 2; dy = dy * 3 / 2; }
    ms_x = CLAMP(ms_x + dx, 0, ms_w - 1);
    ms_y = CLAMP(ms_y - dy, 0, ms_h - 1);
    int btn = ms_pkt[0] & 7;
    if (mouse_swap_buttons) btn = (btn & 4) | ((btn & 1) << 1) | ((btn & 2) >> 1);
    ms_buttons = btn;
    if (ms_pkt_size == 4) { int8_t wz = (int8_t)(ms_pkt[3] << 4) >> 4; ms_wheel += wz; }
    ms_changed = true;
}
/* The keyboard controller has one output buffer for both devices; a byte left
 * in it blocks every later byte and interrupt. So both interrupts (and a timer
 * poll, in case an interrupt is ever lost) drain it, routing each byte by the
 * "mouse data" status bit. */
static void ps2_service(void) {
    for (int i = 0; i < 32; i++) {
        uint8_t st = inb(0x64);
        if (!(st & 1)) break;
        if ((st & 0x20) && !ps2_ready) break;   /* mouse_init is reading its replies */
        uint8_t b = inb(0x60);
        if (st & 0x20) mouse_byte(b); else kb_byte(b);
    }
}
static void mouse_irq(regs_t *r) { (void)r; ps2_service(); }
void ps2_poll(void) { if (ps2_ready) ps2_service(); }
void mouse_set_bounds(int w, int h) { ms_w = w; ms_h = h; if (ms_x >= w) ms_x = w - 1; if (ms_y >= h) ms_y = h - 1; }
void mouse_init(int w, int h) {
    ms_w = w; ms_h = h; ms_x = w / 2; ms_y = h / 2;
    ps2_wait_write(); outb(0x64, 0xA8);
    ps2_wait_write(); outb(0x64, 0x20);
    ps2_wait_read(); uint8_t cfg = inb(0x60);
    cfg |= 2; cfg &= ~0x20;
    ps2_wait_write(); outb(0x64, 0x60);
    ps2_wait_write(); outb(0x60, cfg);
    mouse_write(0xF6); mouse_read();
    /* try to enable the scroll wheel (IntelliMouse) */
    mouse_write(0xF3); mouse_read(); mouse_write(200); mouse_read();
    mouse_write(0xF3); mouse_read(); mouse_write(100); mouse_read();
    mouse_write(0xF3); mouse_read(); mouse_write(80); mouse_read();
    mouse_write(0xF2); mouse_read();
    uint8_t id = mouse_read();
    if (id == 3) ms_pkt_size = 4;
    mouse_write(0xF4); mouse_read();
    irq_install(12, mouse_irq);
    ps2_ready = true;
    klog("PS/2 mouse initialised (id=%d, %s)", id, ms_pkt_size == 4 ? "wheel" : "3-button");
}
bool mouse_poll(mouse_state_t *st) {
    uint32_t f = irq_save();
    bool c = ms_changed;
    st->x = ms_x; st->y = ms_y; st->buttons = ms_buttons; st->wheel = ms_wheel;
    ms_wheel = 0; ms_changed = false;
    irq_restore(f);
    return c;
}
void mouse_warp(int x, int y) { uint32_t f = irq_save(); ms_x = x; ms_y = y; ms_changed = true; irq_restore(f); }
