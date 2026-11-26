/* WRLD Security Center: one place that shows how the computer is protected,
 * in the spirit of XP SP2's Security Center. The home page lists four
 * security essentials with a status each (firewall, account password, web
 * protection, system health); every status comes from the live system, and
 * each essential opens its own settings page. */
#include "kernel.h"
#include "gfx.h"
#include "theme.h"
#include "wm.h"
#include "apps.h"
#include "net.h"
#include "disk.h"
#include "raster.h"
#include "devmgr.h"

extern bool adblock_enabled;
extern uint32_t adblock_total;
int adblock_rule_count(void);
#define ADBLOCK_LIST "C:\\WRLD\\adblock.txt"

enum { PG_HOME, PG_FIREWALL, PG_ACCOUNT, PG_WEB };
enum { ST_GOOD, ST_WARN, ST_BAD };
typedef struct {
    int page, open;                     /* page shown; which essential is expanded (-1 none) */
    char port[8], p1[32], p2[32];
    char msg[96]; uint32_t msg_until; bool msg_bad;
} sec_t;

#define HEAD_H 76
#define PANE_W 196

/* brand colours: the header is the same in every theme */
static const uint32_t SC_HEAD_A = 0xFF0B2A6E, SC_HEAD_B = 0xFF2458B8, SC_HEAD_LINE = 0xFF6F9BE8;
static uint32_t st_col(int st) { return st == ST_GOOD ? HEX(0x2E8B3A) : st == ST_WARN ? HEX(0xD08A00) : HEX(0xC8342A); }

/* ---------------- drawing helpers ---------------- */
/* a shield, w x h at (x, y), with a tick (good), an exclamation mark (warn) or a cross (bad) */
static void shield(canvas_t *c, int x, int y, int w, int h, int st) {
    raster_path_t *p = raster_path_new();
    if (!p) return;
    int32_t X = RASTER_FIX(x), Y = RASTER_FIX(y), W = RASTER_FIX(w), H = RASTER_FIX(h);
    raster_move_to(p, X + W / 2, Y);
    raster_cubic_to(p, X + W * 7 / 10, Y + H / 10, X + W * 9 / 10, Y + H / 10, X + W, Y + H / 12);
    raster_line_to(p, X + W, Y + H * 9 / 20);
    raster_cubic_to(p, X + W, Y + H * 3 / 4, X + W * 7 / 10, Y + H * 9 / 10, X + W / 2, Y + H);
    raster_cubic_to(p, X + W * 3 / 10, Y + H * 9 / 10, X, Y + H * 3 / 4, X, Y + H * 9 / 20);
    raster_line_to(p, X, Y + H / 12);
    raster_cubic_to(p, X + W / 10, Y + H / 10, X + W * 3 / 10, Y + H / 10, X + W / 2, Y);
    raster_close(p);
    uint32_t base = st_col(st);
    raster_fill_gradient(c, p, x + w / 3, y + h / 4, x + w, y + h, lighten(base, 70), darken(base, 40), true, false);
    raster_stroke(c, p, MAX(1, w / 20), darken(base, 70));
    raster_reset(p);
    int sw = MAX(2, w / 9);
    if (st == ST_GOOD) {
        raster_move_to(p, X + W * 27 / 100, Y + H * 50 / 100);
        raster_line_to(p, X + W * 44 / 100, Y + H * 66 / 100);
        raster_line_to(p, X + W * 74 / 100, Y + H * 34 / 100);
        raster_stroke(c, p, sw, WHITE);
    } else if (st == ST_WARN) {
        raster_move_to(p, X + W / 2, Y + H * 26 / 100); raster_line_to(p, X + W / 2, Y + H * 56 / 100);
        raster_stroke(c, p, sw, WHITE);
        fill_circle(c, x + w / 2, y + h * 71 / 100, MAX(1, sw * 2 / 3), WHITE);
    } else {
        raster_move_to(p, X + W * 32 / 100, Y + H * 32 / 100); raster_line_to(p, X + W * 68 / 100, Y + H * 66 / 100);
        raster_move_to(p, X + W * 68 / 100, Y + H * 32 / 100); raster_line_to(p, X + W * 32 / 100, Y + H * 66 / 100);
        raster_stroke(c, p, sw, WHITE);
    }
    raster_path_free(p);
}
/* a status pill: coloured dot and word */
static int pill(canvas_t *c, int right, int cy, const char *word, int st) {
    int tw = text_width(FONT_UI_BOLD, word), w = tw + 30, x = right - w;
    uint32_t col = st_col(st);
    fill_round_rect(c, x, cy - 11, w, 22, 11, mix(col, T.window_bg, dark_mode ? 150 : 205));
    draw_round_rect(c, x, cy - 11, w, 22, 11, mix(col, T.window_bg, 90));
    fill_circle(c, x + 12, cy, 4, col);
    draw_text(c, FONT_UI_BOLD, x + 21, cy - font_height(FONT_UI_BOLD) / 2, word, dark_mode ? lighten(col, 90) : darken(col, 30));
    return w;
}
static void flash(sec_t *s, const char *m, bool bad) { strlcpy(s->msg, m, sizeof s->msg); s->msg_bad = bad; s->msg_until = timer_ticks() + 4500; }
static void ago(uint32_t when, char *out, int cap) {
    uint32_t sec = (timer_ticks() - when) / 1000;
    if (sec < 60) snprintf(out, cap, "%u s ago", sec);
    else if (sec < 3600) snprintf(out, cap, "%u min ago", sec / 60);
    else snprintf(out, cap, "%u h %u min ago", sec / 3600, sec % 3600 / 60);
}

/* ---------------- the four essentials ---------------- */
typedef struct { const char *title, *state; int st; char detail[260]; } essential_t;
static void essentials(essential_t e[4]) {
    char b[96];
    e[0].title = "Firewall";
    e[0].st = fw_enabled ? ST_GOOD : ST_BAD; e[0].state = fw_enabled ? "ON" : "OFF";
    if (fw_enabled) {
        int o = snprintf(e[0].detail, sizeof e[0].detail, "The firewall refuses connections nobody on this computer asked for. It has blocked %u attempt%s since start-up.",
                         fw_blocked, fw_blocked == 1 ? "" : "s");
        if (fw_port_count()) { int k = 0; for (int i = 0; i < fw_port_count() && k < (int)sizeof b - 8; i++) k += snprintf(b + k, sizeof b - k, "%s%u", i ? ", " : "", fw_port_at(i));
            snprintf(e[0].detail + o, sizeof e[0].detail - o, " Ports open to other computers: %s.", b); }
    } else snprintf(e[0].detail, sizeof e[0].detail, "The firewall is off, so other computers on the network can start connections to this one. Turn it on unless a program you run needs to accept them.");
    e[1].title = "Account protection";
    e[1].st = user_password[0] ? ST_GOOD : ST_WARN; e[1].state = user_password[0] ? "ON" : "NO PASSWORD";
    snprintf(e[1].detail, sizeof e[1].detail, user_password[0]
        ? "Your account has a password, so the log-on screen asks for it. It is stored as a salted digest, never as the password itself."
        : "Your account has no password: anyone at this computer can log on as %s. Set one to protect your files and settings.", user_name);
    e[2].title = "Web protection";
    int roots = tls_trust_anchor_count();
    e[2].st = adblock_enabled && roots > 0 ? ST_GOOD : ST_WARN; e[2].state = adblock_enabled ? "ON" : "PARTLY ON";
    snprintf(e[2].detail, sizeof e[2].detail, "Secure websites are checked against %d trusted root certificates. %s", roots,
             adblock_enabled ? "The ad and tracker blocker is on" : "The ad and tracker blocker is off, so trackers and ads load on web pages");
    if (adblock_enabled) { size_t l = strlen(e[2].detail); snprintf(e[2].detail + l, sizeof e[2].detail - l, " and has refused %u request%s since start-up.", adblock_total, adblock_total == 1 ? "" : "s"); }
    else strlcat(e[2].detail, ".", sizeof e[2].detail);
    e[3].title = "System health";
    int probs = devmgr_problem_count();
    bool shell_ok = gui_task != NULL;
    e[3].st = !shell_ok ? ST_BAD : probs ? ST_WARN : ST_GOOD;
    e[3].state = !shell_ok ? "SHELL STOPPED" : probs ? "CHECK DEVICES" : "OK";
    snprintf(e[3].detail, sizeof e[3].detail, "The shell manager is %s. %s", shell_ok ? "running" : "not running",
             probs ? "" : "Every device in this computer has a working driver.");
    if (probs) { size_t l = strlen(e[3].detail); snprintf(e[3].detail + l, sizeof e[3].detail - l, "%d device%s ha%s no working driver; Device Manager lists %s.", probs, probs == 1 ? "" : "s", probs == 1 ? "s" : "ve", probs == 1 ? "it" : "them"); }
}

/* ---------------- frame: header and task pane ---------------- */
static void header(canvas_t *c, int worst, const char *sub) {
    gradient_v(c, 0, 0, c->w, HEAD_H, SC_HEAD_A, SC_HEAD_B);
    for (int i = 0; i < 3; i++) blend_rect(c, c->w - 220 + i * 60, 0, 34, HEAD_H, WHITE, 6);   /* faint light bands */
    hline(c, 0, HEAD_H - 2, c->w, SC_HEAD_LINE); hline(c, 0, HEAD_H - 1, c->w, darken(SC_HEAD_A, 40));
    shield(c, 18, 10, 46, 54, worst);
    draw_text(c, FONT_XL, 78, 12, "WRLD Security Center", WHITE);
    draw_text(c, FONT_UI, 80, 48, sub, HEX(0xC8D8F8));
}
static void pane(window_t *w, sec_t *s, int worst, int problems) {
    canvas_t *c = w->surf;
    int x = 0, y = HEAD_H, h = c->h - HEAD_H;
    if (T.classic) fill_rect(c, x, y, PANE_W, h, T.face_dark);
    else gradient_v(c, x, y, PANE_W, h, T.pane_top, T.pane_bot);
    int bx = 12, bw = PANE_W - 24, yy = y + 14;
    /* overall verdict */
    fill_round_rect(c, bx, yy, bw, 74, 5, T.pane_body);
    shield(c, bx + 10, yy + 10, 26, 31, worst);
    const char *verdict = worst == ST_GOOD ? "Your computer is protected" : problems == 1 ? "1 item needs your attention" : "Some items need your attention";
    draw_text_wrapped(c, FONT_UI_BOLD, bx + 44, yy + 10, bw - 52, verdict, T.text, 2);
    draw_text_wrapped(c, FONT_SMALL, bx + 44, yy + 44, bw - 52, worst == ST_GOOD ? "All four essentials are on." : "Open the item marked in the list.", T.gray_text, 1);
    yy += 90;
    /* resources */
    fill_round_rect(c, bx, yy, bw, 24, 4, T.pane_head_bot);
    draw_text(c, FONT_UI_BOLD, bx + 10, yy + 5, "Resources", T.pane_head_text);
    yy += 24;
    static const char *links[] = { "Device Manager", "Network connections", "Administrative Tools", "Kernel log (dmesg)", "Restart the shell manager" };
    fill_rect(c, bx, yy, bw, 22 * (int)ARRAY_LEN(links) + 10, T.pane_body);
    yy += 6;
    for (unsigned i = 0; i < ARRAY_LEN(links); i++, yy += 22) {
        if (!ui_link(w, bx + 12, yy, FONT_UI, links[i], T.pane_link)) continue;
        if (i == 0) control_open("devices");
        else if (i == 1) netconn_open();
        else if (i == 2) control_open("admin");
        else if (i == 3) { vfs_write_file("C:\\tmp\\dmesg.log", klog_buffer(), strlen(klog_buffer())); notepad_open("C:\\tmp\\dmesg.log"); }
        else shellmanager_restart();
    }
    yy += 14;
    if (s->page != PG_HOME) {
        fill_round_rect(c, bx, yy, bw, 24, 4, T.pane_head_bot);
        draw_text(c, FONT_UI_BOLD, bx + 10, yy + 5, "Security Center", T.pane_head_text);
        yy += 24; fill_rect(c, bx, yy, bw, 32, T.pane_body);
        if (ui_link(w, bx + 12, yy + 8, FONT_UI, "Back to the overview", T.pane_link)) { s->page = PG_HOME; w->dirty = true; }
    }
}

/* ---------------- pages ---------------- */
static void page_home(window_t *w, sec_t *s, essential_t *e, int x0) {
    canvas_t *c = w->surf;
    int cw = c->w - x0 - 22, y = HEAD_H + 16;
    draw_text(c, FONT_LARGE_BOLD, x0, y, "Security essentials", T.text); y += 26;
    y += draw_text_wrapped(c, FONT_UI, x0, y, cw, "These keep THE WRLD OS and your files safe. Click an item to see what it means and what to do.", T.gray_text, 2) + 12;
    for (int i = 0; i < 4; i++) {
        bool open = s->open == i;
        int dh = open ? 2 + text_wrapped_height(FONT_UI, cw - 74, e[i].detail, 2) + 10 + 26 + 12 : 0, h = 44 + dh;
        rect_t r = R(x0, y, cw, h);
        uint32_t col = st_col(e[i].st);
        uint32_t bg = mix(col, T.window_bg, dark_mode ? 225 : 238);
        fill_round_rect(c, r.x, r.y, r.w, r.h, 6, bg);
        draw_round_rect(c, r.x, r.y, r.w, r.h, 6, mix(col, T.window_bg, open ? 110 : 170));
        fill_round_rect(c, r.x, r.y, 6, r.h, 3, col);
        bool hot = ui_hover(w, R(r.x, r.y, r.w, 44));
        if (hot && !open) blend_rect(c, r.x + 6, r.y + 1, r.w - 7, 42, WHITE, dark_mode ? 12 : 60);
        if (ui_click_area(w, R(r.x, r.y, r.w, 44))) { s->open = open ? -1 : i; w->dirty = true; }
        shield(c, r.x + 18, r.y + 9, 22, 26, e[i].st);
        draw_text(c, FONT_LARGE_BOLD, r.x + 52, r.y + 12, e[i].title, T.text);
        int pw = pill(c, r.x + r.w - 40, r.y + 22, e[i].state, e[i].st);
        (void)pw;
        draw_arrow(c, r.x + r.w - 24, r.y + 20, open ? 0 : 1, T.gray_text);
        if (open) {
            int dy = r.y + 46;
            dy += draw_text_wrapped(c, FONT_UI, r.x + 52, dy, cw - 74, e[i].detail, T.text, 2) + 10;
            int bx = r.x + 52;
            switch (i) {
            case 0:
                if (ui_button(w, R(bx, dy, 120, 26), fw_enabled ? "Turn off" : "Turn on")) { fw_enabled = !fw_enabled; settings_save(); }
                if (ui_button(w, R(bx + 128, dy, 140, 26), "Firewall settings")) { s->page = PG_FIREWALL; w->dirty = true; }
                break;
            case 1:
                if (ui_button(w, R(bx, dy, 150, 26), user_password[0] ? "Change password" : "Set a password")) { s->page = PG_ACCOUNT; w->dirty = true; }
                break;
            case 2:
                if (ui_button(w, R(bx, dy, 170, 26), adblock_enabled ? "Turn off the blocker" : "Turn on the blocker")) { adblock_enabled = !adblock_enabled; settings_save(); }
                if (ui_button(w, R(bx + 178, dy, 170, 26), "Web protection settings")) { s->page = PG_WEB; w->dirty = true; }
                break;
            case 3:
                if (ui_button(w, R(bx, dy, 130, 26), "Device Manager")) control_open("devices");
                if (ui_button(w, R(bx + 138, dy, 170, 26), "Restart shell manager")) shellmanager_restart();
                break;
            }
        }
        y += h + 8;
    }
    /* manage settings for */
    y += 10;
    if (y + 90 > c->h) return;
    draw_text(c, FONT_UI_BOLD, x0, y, "Manage security settings for:", T.text);
    hline(c, x0 + text_width(FONT_UI_BOLD, "Manage security settings for:") + 10, y + 8, cw - text_width(FONT_UI_BOLD, "Manage security settings for:") - 10, mix(T.text, T.window_bg, 210));
    y += 24;
    static const struct { const char *label; int icon, page; } items[] = {
        { "Firewall", ICON_CP_NETWORK, PG_FIREWALL }, { "Account password", ICON_CP_USERS, PG_ACCOUNT }, { "Web protection", ICON_BROWSER, PG_WEB } };
    int ix = x0;
    for (unsigned i = 0; i < ARRAY_LEN(items); i++) {
        int lw = text_width(FONT_UI, items[i].label);
        rect_t r = R(ix, y, 40 + lw + 12, 40);
        if (ui_hover(w, r)) fill_round_rect(c, r.x, r.y, r.w, r.h, 4, mix(T.select, T.window_bg, 210));
        draw_icon(c, items[i].icon, 32, ix + 4, y + 4);
        draw_text(c, FONT_UI, ix + 42, y + 12, items[i].label, T.link);
        if (ui_click_area(w, r)) { s->page = items[i].page; w->dirty = true; }
        ix += r.w + 18;
    }
}

static int section(canvas_t *c, int x, int y, int w, const char *t) {
    draw_text(c, FONT_UI_BOLD, x, y, t, T.link_head);
    hline(c, x + text_width(FONT_UI_BOLD, t) + 10, y + 8, w - text_width(FONT_UI_BOLD, t) - 10, mix(T.text, T.window_bg, 210));
    return y + 24;
}
static void page_firewall(window_t *w, sec_t *s, int x0) {
    canvas_t *c = w->surf;
    int cw = c->w - x0 - 22, y = HEAD_H + 16;
    shield(c, x0, y, 26, 31, fw_enabled ? ST_GOOD : ST_BAD);
    draw_text(c, FONT_LARGE_BOLD, x0 + 36, y + 4, "Firewall", T.text); y += 42;
    bool changed = false;
    y = section(c, x0, y, cw, "General");
    if (ui_checkbox(w, x0 + 8, y, "On: refuse connections nobody on this computer asked for", &fw_enabled)) changed = true; y += 24;
    if (ui_checkbox(w, x0 + 28, y, "Block new incoming TCP connections", &fw_block_tcp)) changed = true; y += 22;
    if (ui_checkbox(w, x0 + 28, y, "Block UDP datagrams to ports no program is using", &fw_block_udp)) changed = true; y += 30;
    y = section(c, x0, y, cw, "Exceptions: ports open to other computers");
    int listed = 0;
    for (int i = 0; i < fw_port_count(); i++) {
        char b[48]; uint16_t port = fw_port_at(i);
        snprintf(b, sizeof b, "Port %u (TCP and UDP)", port);
        draw_icon(c, ICON_CP_NETWORK, 16, x0 + 8, y + 3);
        draw_text(c, FONT_UI, x0 + 30, y + 4, b, T.text);
        if (ui_button(w, R(x0 + 230, y, 80, 22), "Remove")) { fw_remove_port(port); changed = true; }
        y += 26; listed++;
    }
    if (!listed) { draw_text(c, FONT_UI, x0 + 8, y + 2, "None. Every port is closed to connections from outside.", T.gray_text); y += 24; }
    draw_text(c, FONT_UI, x0 + 8, y + 4, "Open port:", T.text);
    ui_textbox(w, R(x0 + 82, y, 70, 22), s->port, sizeof s->port);
    if (ui_button(w, R(x0 + 160, y - 1, 70, 24), "Add")) {
        int p = atoi(s->port);
        if (p <= 0 || p > 65535) flash(s, "Type a port number from 1 to 65535.", true);
        else if (!fw_allow_port((uint16_t)p)) flash(s, "The exception list is full (16 ports).", true);
        else { s->port[0] = 0; changed = true; flash(s, "Port added.", false); }
    }
    if (ui_button(w, R(x0 + 236, y - 1, 90, 24), "Remove all")) { fw_clear_ports(); changed = true; }
    y += 36;
    char b[96]; snprintf(b, sizeof b, "Blocked attempts (%u since start-up)", fw_blocked);
    y = section(c, x0, y, cw, b);
    static const char *hd[] = { "When", "From", "Port", "Protocol" };
    int cx[] = { x0 + 8, x0 + 130, x0 + 270, x0 + 340 };
    for (int k = 0; k < 4; k++) draw_text(c, FONT_UI_BOLD, cx[k], y, hd[k], T.gray_text);
    y += 20;
    int n = fw_log_count();
    if (!n) draw_text(c, FONT_UI, cx[0], y, fw_enabled ? "Nothing has been blocked yet." : "The firewall is off, so nothing is blocked.", T.gray_text);
    for (int i = 0; i < n && y + 18 < c->h - 8; i++, y += 19) {
        const fw_event_t *e = fw_log_at(i);
        char t[32], ip[16], port[8];
        ago(e->when, t, sizeof t); ip_to_str(e->src, ip); snprintf(port, sizeof port, "%u", e->port);
        if (i % 2) fill_rect(c, x0 + 2, y - 2, cw - 4, 19, mix(T.text, T.window_bg, 245));
        draw_text(c, FONT_UI, cx[0], y, t, T.text); draw_text(c, FONT_UI, cx[1], y, ip, T.text);
        draw_text(c, FONT_UI, cx[2], y, port, T.text); draw_text(c, FONT_UI, cx[3], y, e->proto == IPPROTO_TCP ? "TCP" : "UDP", T.text);
    }
    if (changed) settings_save();
}
static void page_account(window_t *w, sec_t *s, int x0) {
    canvas_t *c = w->surf;
    int cw = c->w - x0 - 22, y = HEAD_H + 16;
    shield(c, x0, y, 26, 31, user_password[0] ? ST_GOOD : ST_WARN);
    draw_text(c, FONT_LARGE_BOLD, x0 + 36, y + 4, "Account password", T.text); y += 42;
    draw_icon(c, user_picture, 48, x0, y);
    draw_text(c, FONT_LARGE_BOLD, x0 + 60, y + 4, user_name, T.text);
    draw_text(c, FONT_UI, x0 + 60, y + 28, user_password[0] ? "Password protected" : "No password: anyone can log on", user_password[0] ? st_col(ST_GOOD) : st_col(ST_WARN));
    y += 66;
    y = section(c, x0, y, cw, user_password[0] ? "Change your password" : "Create a password");
    draw_text(c, FONT_UI, x0 + 8, y + 4, "New password:", T.text);
    ui_password(w, R(x0 + 130, y, 220, 22), s->p1, sizeof s->p1); y += 30;
    draw_text(c, FONT_UI, x0 + 8, y + 4, "Type it again:", T.text);
    ui_password(w, R(x0 + 130, y, 220, 22), s->p2, sizeof s->p2); y += 34;
    if (ui_button(w, R(x0 + 130, y, 130, 26), user_password[0] ? "Change password" : "Create password")) {
        if (!s->p1[0]) flash(s, "Type a password first.", true);
        else if (strcmp(s->p1, s->p2)) flash(s, "The two passwords are different. Type them again.", true);
        else { password_set(s->p1); s->p1[0] = s->p2[0] = 0; settings_save(); flash(s, "Password saved. The log-on screen will ask for it.", false); }
    }
    if (user_password[0] && ui_button(w, R(x0 + 268, y, 140, 26), "Remove password")) { password_set(""); settings_save(); flash(s, "Password removed.", false); }
    y += 44;
    y = section(c, x0, y, cw, "How the password is kept");
    draw_text_wrapped(c, FONT_UI, x0 + 8, y, cw - 16, "THE WRLD OS keeps only a salted digest of the password in C:\\WRLD\\wrld.ini, so the password itself is never written to the disk. "
                      "The Command Prompt's passwd command changes it too.", T.gray_text, 2);
}
static void page_web(window_t *w, sec_t *s, int x0) {
    canvas_t *c = w->surf;
    int cw = c->w - x0 - 22, y = HEAD_H + 16;
    shield(c, x0, y, 26, 31, adblock_enabled ? ST_GOOD : ST_WARN);
    draw_text(c, FONT_LARGE_BOLD, x0 + 36, y + 4, "Web protection", T.text); y += 42;
    y = section(c, x0, y, cw, "Ads and trackers");
    if (ui_checkbox(w, x0 + 8, y, "Block ads and trackers on web pages", &adblock_enabled)) settings_save();
    y += 26;
    char b[160];
    snprintf(b, sizeof b, "%u requests refused since start-up, using %d rules. Sites on your allow list are not filtered.", adblock_total, adblock_rule_count());
    y += draw_text_wrapped(c, FONT_UI, x0 + 28, y, cw - 36, b, T.gray_text, 2) + 8;
    if (ui_button(w, R(x0 + 28, y, 200, 26), "Edit your block and allow list")) notepad_open(ADBLOCK_LIST);
    y += 44;
    y = section(c, x0, y, cw, "Secure connections");
    snprintf(b, sizeof b, "Websites that use HTTPS are checked against %d trusted root certificates (TLS 1.3 and 1.2). A site whose certificate does not check out is not opened.",
             tls_trust_anchor_count());
    y += draw_text_wrapped(c, FONT_UI, x0 + 8, y, cw - 16, b, T.text, 2) + 8;
    y += draw_text_wrapped(c, FONT_UI, x0 + 8, y, cw - 16, "To trust a certificate of your own (a home server, for example), copy its .crt or .pem file to C:\\WRLD\\Certificates.", T.gray_text, 2) + 8;
    if (ui_button(w, R(x0 + 8, y, 190, 26), "Open the Certificates folder")) { vfs_mkdir_p("C:\\WRLD\\Certificates"); explorer_open("C:\\WRLD\\Certificates"); }
    (void)s;
}

static void sec_paint(window_t *w) {
    sec_t *s = w->data;
    canvas_t *c = w->surf;
    essential_t e[4];
    essentials(e);
    int worst = ST_GOOD, problems = 0;
    for (int i = 0; i < 4; i++) { if (e[i].st != ST_GOOD) problems++; if (e[i].st > worst) worst = e[i].st; }
    gfx_clear(c, T.window_bg);
    static const char *subs[] = { "Help protect your computer", "Firewall", "Account password", "Web protection" };
    header(c, worst, subs[s->page]);
    pane(w, s, worst, problems);
    int x0 = PANE_W + 22;
    rect_t old = canvas_push_clip(c, R(PANE_W, HEAD_H, c->w - PANE_W, c->h - HEAD_H));
    if (s->page == PG_FIREWALL) page_firewall(w, s, x0);
    else if (s->page == PG_ACCOUNT) page_account(w, s, x0);
    else if (s->page == PG_WEB) page_web(w, s, x0);
    else page_home(w, s, e, x0);
    if (s->msg[0] && (int32_t)(timer_ticks() - s->msg_until) < 0) {
        int tw = text_width(FONT_UI, s->msg) + 24, bx = c->w - tw - 18, by = c->h - 40;
        fill_round_rect(c, bx, by, tw, 28, 5, s->msg_bad ? mix(st_col(ST_BAD), T.window_bg, 200) : mix(st_col(ST_GOOD), T.window_bg, 200));
        draw_text(c, FONT_UI, bx + 12, by + 7, s->msg, T.text);
    }
    c->clip = old;
}
static void sec_tick(window_t *w) { w->dirty = true; }
static void sec_destroy(window_t *w) { kfree(w->data); }
void security_open(void) {
    window_t *w = wm_create("WRLD Security Center", -1, -1, MIN(800, SCREEN_W - 40), MIN(600, SCREEN_H - 70), WF_RESIZABLE);
    sec_t *s = kzalloc(sizeof *s);
    s->open = -1;
    w->data = s; w->paint = sec_paint; w->destroy = sec_destroy; w->icon = ICON_CP_SYSTEM;
    w->min_w = 660; w->min_h = 500;
    wm_set_timer(w, 1000, sec_tick);
    wm_center(w);
}
