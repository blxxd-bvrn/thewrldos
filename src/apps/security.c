/* Security Center: inbound firewall, account password and shell status. */
#include "kernel.h"
#include "gfx.h"
#include "theme.h"
#include "wm.h"
#include "apps.h"
#include "net.h"
#include "disk.h"

typedef struct { char port[8], p1[32], p2[32]; char msg[96]; uint32_t msg_until; } sec_t;

static void sec_paint(window_t *w) {
    sec_t *s = w->data;
    canvas_t *c = w->surf;
    gfx_clear(c, T.face);
    gradient_h(c, 0, 0, c->w, 34, T.pane_head_bot, T.face);
    draw_text(c, FONT_LARGE_BOLD, 14, 7, "Security Center", T.pane_head_text);
    int y = 48;
    draw_group(c, R(14, y, c->w - 28, 190), "Firewall");
    y += 24;
    bool changed = false;
    if (ui_checkbox(w, 28, y, "Firewall enabled", &fw_enabled)) changed = true; y += 24;
    if (ui_checkbox(w, 28, y, "Block new inbound TCP connections", &fw_block_tcp)) changed = true; y += 24;
    if (ui_checkbox(w, 28, y, "Block unsolicited UDP datagrams", &fw_block_udp)) changed = true; y += 26;
    draw_text(c, FONT_UI, 28, y, "Allow inbound port:", T.text);
    ui_textbox(w, R(160, y - 3, 70, 22), s->port, sizeof s->port);
    if (ui_button(w, R(238, y - 4, 90, 24), "Allow")) {
        int p = atoi(s->port);
        if (p > 0 && p < 65536) { fw_allow_port((uint16_t)p); s->port[0] = 0; changed = true; }
    }
    if (ui_button(w, R(334, y - 4, 80, 24), "Clear")) { fw_clear_ports(); changed = true; }
    y += 28;
    char b[160]; int o = snprintf(b, sizeof b, "Allowed: ");
    for (int i = 0; i < fw_port_count() && o < (int)sizeof b - 8; i++) o += snprintf(b + o, sizeof b - o, "%s%u", i ? ", " : "", fw_port_at(i));
    draw_text_ellipsis(c, FONT_SMALL, 28, y, c->w - 56, fw_port_count() ? b : "none", T.gray_text); y += 18;
    snprintf(b, sizeof b, "Blocked packets: %u", fw_blocked);
    draw_text(c, FONT_SMALL, 28, y, b, T.gray_text);
    if (changed) settings_save();

    y += 30;
    draw_group(c, R(14, y, c->w - 28, 132), "Account and sign-in");
    y += 24;
    draw_text(c, FONT_UI, 28, y, user_password[0] ? "Password: set" : "Password: not set", T.text); y += 24;
    draw_text(c, FONT_UI, 28, y, "New password:", T.text);
    ui_password(w, R(140, y - 3, 220, 22), s->p1, sizeof s->p1); y += 26;
    draw_text(c, FONT_UI, 28, y, "Confirm:", T.text);
    ui_password(w, R(140, y - 3, 220, 22), s->p2, sizeof s->p2); y += 28;
    if (ui_button(w, R(28, y, 90, 24), "Set password")) {
        if (strcmp(s->p1, s->p2)) strlcpy(s->msg, "The passwords do not match.", sizeof s->msg);
        else { password_set(s->p1); s->p1[0] = s->p2[0] = 0; strlcpy(s->msg, "Password saved.", sizeof s->msg); settings_save(); }
        s->msg_until = timer_ticks() + 4000;
    }
    if (ui_button(w, R(126, y, 110, 24), "Clear password")) { password_set(""); settings_save(); strlcpy(s->msg, "Password cleared.", sizeof s->msg); s->msg_until = timer_ticks() + 4000; }
    if (s->msg[0] && (int32_t)(timer_ticks() - s->msg_until) < 0) draw_text(c, FONT_SMALL, 250, y + 6, s->msg, T.gray_text);

    y += 38;
    draw_group(c, R(14, y, c->w - 28, 92), "Security layers");
    y += 24;
    snprintf(b, sizeof b, "Firewall: %s    Account password: %s    Shell manager: %s", fw_enabled ? "on" : "off",
             user_password[0] ? "set" : "not set", gui_task ? "running" : "not running");
    draw_text_wrapped(c, FONT_UI, 28, y, c->w - 56, b, T.text, 3);
    y += 32;
    if (ui_button(w, R(28, y, 150, 24), "Restart shell manager")) shellmanager_restart();
}

static void sec_destroy(window_t *w) { kfree(w->data); }
void security_open(void) {
    window_t *w = wm_create("Security Center", -1, -1, 520, 560, WF_NO_MAX);
    sec_t *s = kzalloc(sizeof *s);
    w->data = s; w->paint = sec_paint; w->destroy = sec_destroy; w->icon = ICON_CP_SYSTEM;
    wm_center(w);
}
