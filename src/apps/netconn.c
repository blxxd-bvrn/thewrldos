/* Network Connections, as in Windows 7 (Control Panel > Network and Internet > Change adapter
 * settings, or "ncpa.cpl"): the adapter as a tile with its state, a command bar (Disable, Diagnose,
 * Rename, View status, Change settings), and the dialogs behind it:
 *   - Local Area Connection Status: IPv4/IPv6 connectivity, media state, duration, speed, bytes
 *     sent and received, with Details..., Properties, Disable and Diagnose;
 *   - Network Connection Details: the address, mask, gateway, DHCP server and lease, DNS;
 *   - Local Area Connection Properties: the adapter and the items the connection uses;
 *   - Internet Protocol Version 4 (TCP/IPv4) Properties: DHCP or a fixed address and DNS server.
 * Disable, the fixed address and the name are real and saved (net_disabled, net_ip, net_mask,
 * net_gw, net_dns, net_name; applied at start by net_apply_saved). */
#include "kernel.h"
#include "gfx.h"
#include "theme.h"
#include "wm.h"
#include "apps.h"
#include "disk.h"
#include "net.h"

#define DEFAULT_NAME "Local Area Connection"
static const char *conn_name(void) { const char *n = settings_get("net_name"); return n && *n ? n : DEFAULT_NAME; }

/* the saved adapter settings, at start (persist.c calls this after loading the settings) */
void net_apply_saved(void) {
    const char *v;
    uint32_t ip = 0, mask = 0, gw = 0, dns = 0;
    if ((v = settings_get("net_ip")) && *v && str_to_ip(v, &ip)) {
        if ((v = settings_get("net_mask"))) str_to_ip(v, &mask);
        if ((v = settings_get("net_gw"))) str_to_ip(v, &gw);
        if ((v = settings_get("net_dns"))) str_to_ip(v, &dns);
        net_set_static(ip, mask, gw, dns);
    }
    v = settings_get("net_disabled");
    if (v && atoi(v)) net_set_enabled(false);
}

/* ---------------- what the dialogs show ---------------- */
static const char *state_text(void) {
    if (!netif.present) return "No adapter";
    if (!net_enabled()) return "Disabled";
    if (!netif.link) return "Network cable unplugged";
    if (!netif.configured) return "Identifying...";
    return netif.gw ? "Network" : "Unidentified network";
}
static const char *speed_text(void) {
    const char *d = netif.driver;
    if (strstr(d, "8139") || strstr(d, "PCnet") || strstr(d, "pcnet")) return "100.0 Mbps";
    if (strstr(d, "8125")) return "2.5 Gbps";
    if (strstr(d, "VMXNET3") || strstr(d, "vmxnet3")) return "10.0 Gbps";
    return "1.0 Gbps";
}
static void duration_text(char *b, int cap) {
    uint32_t s = net_up_ms() / 1000;
    if (s >= 86400) snprintf(b, cap, "%u days %02u:%02u:%02u", s / 86400, s / 3600 % 24, s / 60 % 60, s % 60);
    else snprintf(b, cap, "%02u:%02u:%02u", s / 3600, s / 60 % 60, s % 60);
}
static void bytes_text(char *b, int cap, uint32_t v) {           /* 1,234,567 */
    char t[16]; int n = snprintf(t, sizeof t, "%u", v), o = 0;
    for (int i = 0; i < n && o < cap - 1; i++) { if (i && (n - i) % 3 == 0 && o < cap - 2) b[o++] = ','; b[o++] = t[i]; }
    b[o] = 0;
}
static void ip_or_dash(uint32_t ip, char *b) { if (ip) ip_to_str(ip, b); else strcpy(b, ""); }

/* the adapter picture: two screens joined by a cable; grey when disabled, a red cross when the
 * cable is out */
static void adapter_icon(canvas_t *s, int x, int y) {
    bool on = net_enabled() && netif.present;
    uint32_t frame = on ? HEX(0x3C4B5C) : HEX(0x9AA3AC), glass1 = on ? HEX(0x8FD0FF) : HEX(0xD8DDE2), glass2 = on ? HEX(0x2D7FCB) : HEX(0xB5BCC4);
    for (int k = 0; k < 2; k++) {
        int mx = x + (k ? 20 : 0), my = y + (k ? 14 : 0);
        fill_round_rect(s, mx, my, 26, 20, 2, frame);
        gradient_v(s, mx + 2, my + 2, 22, 14, glass1, glass2);
        fill_rect(s, mx + 10, my + 20, 6, 3, frame);
        fill_rect(s, mx + 6, my + 23, 14, 2, frame);
    }
    uint32_t cable = on && netif.link ? HEX(0x2E9E3E) : HEX(0x8A9098);
    hline(s, x + 13, y + 30, 8, cable); fill_rect(s, x + 12, y + 25, 2, 6, cable);
    if (netif.present && net_enabled() && !netif.link) {               /* red cross */
        fill_round_rect(s, x + 30, y + 30, 16, 16, 8, HEX(0xD8312B));
        for (int i = 0; i < 8; i++) { fill_rect(s, x + 34 + i, y + 34 + i, 2, 2, WHITE); fill_rect(s, x + 41 - i, y + 34 + i, 2, 2, WHITE); }
    }
}

/* ---------------- Diagnose: what Windows Network Diagnostics checks, in a task ---------------- */
static char diag_msg[400]; static bool diag_running;
static void diag_show(void *arg) { (void)arg; msgbox("Windows Network Diagnostics", diag_msg, MBI_INFO, MBX_OK, NULL, NULL); diag_running = false; }
static void diag_task(void *arg) {
    (void)arg;
    char g[16], err[96];
    uint32_t ip;
    if (!netif.present) snprintf(diag_msg, sizeof diag_msg, "No network adapter was found.\n\nSee Device Manager for the network hardware and its driver.");
    else if (!net_enabled()) snprintf(diag_msg, sizeof diag_msg, "%s is disabled.\n\nEnable it in Network Connections.", conn_name());
    else if (!netif.link) snprintf(diag_msg, sizeof diag_msg, "The network cable is unplugged, or the adapter has no link.\n\nCheck the cable (in a virtual machine: that the network adapter is connected).");
    else if (!netif.configured) { dhcp_renew(); snprintf(diag_msg, sizeof diag_msg, "%s does not have an IP address yet.\n\nIt asked the DHCP server again; try once more in a few seconds.", conn_name()); }
    else if (netif.gw && (ip_to_str(netif.gw, g), icmp_ping(netif.gw, 1, 2000) < 0) && icmp_ping(netif.gw, 2, 2000) < 0)
        snprintf(diag_msg, sizeof diag_msg, "The default gateway (%s) is not answering.\n\nIt may block pings; if web pages also fail, check the router or the virtual machine's network setting.", g);
    else if (!dns_resolve("www.example.com", &ip, err, sizeof err))
        snprintf(diag_msg, sizeof diag_msg, "The DNS server is not responding (%s).\n\nTry another DNS server in Internet Protocol Version 4 (TCP/IPv4) Properties.", err);
    else snprintf(diag_msg, sizeof diag_msg, "No problems were found.\n\n%s has an address, the gateway answers and names can be looked up.", conn_name());
    wm_defer(diag_show, NULL);
}
static void diagnose(void) {
    if (diag_running) return;
    diag_running = true;
    task_create("netdiag", diag_task, NULL, 32 * 1024);
}

/* disable/enable, saved */
static void set_enabled(bool on) { net_set_enabled(on); settings_set("net_disabled", on ? "0" : "1"); settings_save(); }

/* ---------------- Internet Protocol Version 4 (TCP/IPv4) Properties ---------------- */
typedef struct { bool fixed, dns_fixed; char ip[16], mask[16], gw[16], dns[16], alt[16]; char err[96]; } ip4_t;
static window_t *ip4_win;
static void ip4_field(window_t *w, int x, int y, const char *label, char *buf, bool on) {
    canvas_t *s = w->surf;
    draw_text(s, FONT_UI, x, y + 4, label, on ? T.text : T.gray_text);
    rect_t r = R(x + 170, y, 130, 22);
    if (on) ui_textbox(w, r, buf, 16);
    else { draw_sunken_box(s, r, T.face); draw_text(s, FONT_UI, r.x + 4, r.y + 4, buf, T.gray_text); }
}
static void ip4_paint(window_t *w) {
    ip4_t *o = w->data; canvas_t *s = w->surf;
    gfx_clear(s, T.face);
    ui_tab(w, R(6, 6, 70, 22), "General", true);
    rect_t body = R(6, 28, s->w - 12, s->h - 72);
    fill_rect(s, body.x, body.y, body.w, body.h, T.classic ? T.face : T.window_bg);
    draw_rect(s, body.x, body.y, body.w, body.h, T.face_dark);
    int x = 18, y = 40;
    y += draw_text_wrapped(s, FONT_UI, x, y, s->w - 40, "You can get IP settings assigned automatically if your network supports this capability. Otherwise, you need to ask your network administrator for the appropriate IP settings.", T.text, 2) + 12;
    if (ui_radio(w, x, y, "Obtain an IP address automatically", !o->fixed)) { o->fixed = false; o->dns_fixed = false; }
    y += 24;
    if (ui_radio(w, x, y, "Use the following IP address:", o->fixed)) { o->fixed = true; o->dns_fixed = true; }
    y += 26;
    draw_round_rect(s, x, y - 4, s->w - 42, 96, 3, T.face_dark);
    ip4_field(w, x + 14, y + 6, "IP address:", o->ip, o->fixed);
    ip4_field(w, x + 14, y + 34, "Subnet mask:", o->mask, o->fixed);
    ip4_field(w, x + 14, y + 62, "Default gateway:", o->gw, o->fixed);
    y += 108;
    if (ui_radio(w, x, y, "Obtain DNS server address automatically", !o->dns_fixed) && !o->fixed) o->dns_fixed = false;
    y += 24;
    if (ui_radio(w, x, y, "Use the following DNS server addresses:", o->dns_fixed)) o->dns_fixed = true;
    y += 26;
    draw_round_rect(s, x, y - 4, s->w - 42, 68, 3, T.face_dark);
    ip4_field(w, x + 14, y + 6, "Preferred DNS server:", o->dns, o->dns_fixed);
    ip4_field(w, x + 14, y + 34, "Alternate DNS server:", o->alt, false);
    if (o->err[0]) draw_text_wrapped(s, FONT_UI, x, body.y + body.h - 22, s->w - 40, o->err, HEX(0xC03020), 1);
    int by = s->h - 36;
    if (ui_button_ex(w, R(s->w - 168, by, 76, 24), "OK", true, true)) {
        uint32_t ip = 0, mask = 0, gw = 0, dns = 0;
        if (o->fixed) {
            if (!str_to_ip(o->ip, &ip) || !ip) { strcpy(o->err, "The IP address is not valid."); return; }
            if (!str_to_ip(o->mask, &mask) || !mask) { strcpy(o->err, "The subnet mask is not valid."); return; }
            if (o->gw[0] && !str_to_ip(o->gw, &gw)) { strcpy(o->err, "The default gateway is not valid."); return; }
        }
        if (o->dns_fixed && o->dns[0] && !str_to_ip(o->dns, &dns)) { strcpy(o->err, "The DNS server address is not valid."); return; }
        settings_set("net_ip", o->fixed ? o->ip : ""); settings_set("net_mask", o->fixed ? o->mask : "");
        settings_set("net_gw", o->fixed ? o->gw : ""); settings_set("net_dns", o->dns_fixed ? o->dns : "");
        settings_save();
        net_set_static(o->fixed ? ip : 0, mask, gw, dns);
        wm_destroy(w); return;
    }
    if (ui_button(w, R(s->w - 86, by, 76, 24), "Cancel")) wm_destroy(w);
}
static void ip4_destroy(window_t *w) { kfree(w->data); w->data = NULL; ip4_win = NULL; }
static void ip4_open(void) {
    if (ip4_win && wm_alive(ip4_win)) { wm_focus(ip4_win); return; }
    ip4_t *o = kzalloc(sizeof *o);
    o->fixed = net_is_static();
    const char *v = settings_get("net_dns"); o->dns_fixed = o->fixed || (v && *v);
    ip_or_dash(netif.ip, o->ip); ip_or_dash(netif.mask, o->mask); ip_or_dash(netif.gw, o->gw);
    if (v && *v) strlcpy(o->dns, v, sizeof o->dns); else ip_or_dash(netif.dns, o->dns);
    window_t *w = wm_create("Internet Protocol Version 4 (TCP/IPv4) Properties", -1, -1, 400, 470, WF_NO_MAX | WF_NO_MIN);
    w->data = o; w->paint = ip4_paint; w->destroy = ip4_destroy; w->icon = ICON_CP_NETWORK;
    ip4_win = w; wm_center(w);
}

/* ---------------- Local Area Connection Properties ---------------- */
static window_t *prop_win; static int prop_sel = 1;
static void prop_paint(window_t *w) {
    canvas_t *s = w->surf;
    gfx_clear(s, T.face);
    ui_tab(w, R(6, 6, 84, 22), "Networking", true);
    rect_t body = R(6, 28, s->w - 12, s->h - 72);
    fill_rect(s, body.x, body.y, body.w, body.h, T.classic ? T.face : T.window_bg);
    draw_rect(s, body.x, body.y, body.w, body.h, T.face_dark);
    int x = 18, y = 40, wd = s->w - 36;
    draw_text(s, FONT_UI, x, y, "Connect using:", T.text); y += 20;
    draw_sunken_box(s, R(x, y, wd, 26), T.window_bg);
    draw_icon(s, ICON_CP_NETWORK, 16, x + 6, y + 5);
    draw_text_ellipsis(s, FONT_UI, x + 28, y + 6, wd - 36, netif.present ? netif.driver : "None", T.text);
    y += 32;
    if (ui_button(w, R(s->w - 104, y, 86, 24), "Configure...")) control_open("devices");
    y += 34;
    draw_text(s, FONT_UI, x, y, "This connection uses the following items:", T.text); y += 20;
    static const char *items[] = { "WRLD Firewall", "Internet Protocol Version 4 (TCP/IPv4)", "Internet Protocol Version 6 (TCP/IPv6)", "Link-Layer Topology Discovery Responder" };
    static const bool on[] = { true, true, false, false };
    rect_t lr = R(x, y, wd, 4 * 22 + 4);
    draw_sunken_box(s, lr, T.window_bg);
    for (int i = 0; i < 4; i++) {
        rect_t r = R(lr.x + 2, lr.y + 2 + i * 22, lr.w - 4, 22);
        if (i == prop_sel) fill_rect(s, r.x, r.y, r.w, r.h, T.select);
        uint32_t col = i == prop_sel ? T.select_text : (i < 2 ? T.text : T.gray_text);
        draw_rect(s, r.x + 4, r.y + 5, 12, 12, T.face_dark);
        if (on[i]) { fill_rect(s, r.x + 7, r.y + 10, 2, 4, col); for (int k = 0; k < 5; k++) fill_rect(s, r.x + 9 + k, r.y + 12 - k, 1, 2, col); }
        draw_icon(s, i == 0 ? ICON_LOCK : ICON_CP_NETWORK, 16, r.x + 22, r.y + 3);
        draw_text(s, FONT_UI, r.x + 44, r.y + 4, items[i], col);
        if (ui_hover(w, r) && w->ui.dblclick && i == 1) { w->ui.dblclick = false; ip4_open(); }
        if (ui_click_area(w, r)) { prop_sel = i; w->dirty = true; }
    }
    y = lr.y + lr.h + 8;
    int bw = (wd - 12) / 3;
    ui_button_ex(w, R(x, y, bw, 24), "Install...", false, false);
    ui_button_ex(w, R(x + bw + 6, y, bw, 24), "Uninstall", false, false);
    if (ui_button_ex(w, R(x + 2 * (bw + 6), y, bw, 24), "Properties", prop_sel <= 1, false)) { if (prop_sel == 1) ip4_open(); else security_open(); }
    y += 36;
    draw_group(s, R(x, y, wd, 74), "Description");
    static const char *desc[] = { "Blocks connections from other computers that you did not ask for (set in the Security Center).",
                                  "Transmission Control Protocol/Internet Protocol: the protocol of the Internet and of most networks.",
                                  "The newer version of the Internet Protocol. Not available in this version of " OS_NAME ".",
                                  "Lets other computers discover this one on the network. Not available in this version." };
    draw_text_wrapped(s, FONT_UI, x + 10, y + 20, wd - 20, desc[prop_sel], T.text, 2);
    int by = s->h - 36;
    if (ui_button_ex(w, R(s->w - 168, by, 76, 24), "OK", true, true)) { wm_destroy(w); return; }
    if (ui_button(w, R(s->w - 86, by, 76, 24), "Cancel")) wm_destroy(w);
}
static void prop_destroy(window_t *w) { (void)w; prop_win = NULL; }
static void props_open(void) {
    if (prop_win && wm_alive(prop_win)) { wm_focus(prop_win); return; }
    char t[96]; snprintf(t, sizeof t, "%s Properties", conn_name());
    window_t *w = wm_create(t, -1, -1, 380, 450, WF_NO_MAX | WF_NO_MIN);
    w->paint = prop_paint; w->destroy = prop_destroy; w->icon = ICON_CP_NETWORK;
    prop_win = w; wm_center(w);
}

/* ---------------- Network Connection Details ---------------- */
static window_t *det_win;
static void det_paint(window_t *w) {
    canvas_t *s = w->surf;
    gfx_clear(s, T.face);
    draw_text(s, FONT_UI, 12, 12, "Network Connection Details:", T.text);
    rect_t lr = R(12, 32, s->w - 24, s->h - 80);
    draw_sunken_box(s, lr, T.window_bg);
    int cx = lr.x + 190;
    gradient_v(s, lr.x + 1, lr.y + 1, lr.w - 2, 22, HEX(0xFFFFFF), HEX(0xEEF1F5));
    hline(s, lr.x + 1, lr.y + 23, lr.w - 2, T.face_dark);
    draw_text(s, FONT_UI, lr.x + 8, lr.y + 5, "Property", T.text);
    draw_text(s, FONT_UI, cx + 6, lr.y + 5, "Value", T.text);
    for (int yy = lr.y + 2; yy < lr.y + 22; yy++) fill_rect(s, cx, yy, 1, 1, T.face_dark);
    char ip[16], mask[16], gw[16], dns[16], srv[16], mac[20], lease[48], exp[48];
    ip_to_str(netif.ip, ip); ip_to_str(netif.mask, mask); ip_to_str(netif.gw, gw); ip_to_str(netif.dns, dns); ip_to_str(netif.dhcp_server, srv);
    snprintf(mac, sizeof mac, "%02X-%02X-%02X-%02X-%02X-%02X", netif.mac[0], netif.mac[1], netif.mac[2], netif.mac[3], netif.mac[4], netif.mac[5]);
    bool dhcp = !net_is_static();
    uint32_t up = net_up_ms() / 1000;
    if (dhcp && netif.lease_s && netif.configured) {
        snprintf(lease, sizeof lease, "%u minutes ago", up / 60);
        if (netif.lease_s >= up) snprintf(exp, sizeof exp, "in %u hours %u minutes", (netif.lease_s - up) / 3600, (netif.lease_s - up) / 60 % 60);
        else strcpy(exp, "now");
    } else { lease[0] = exp[0] = 0; }
    const char *rows[][2] = {
        { "Connection-specific DNS Suffix", "" }, { "Description", netif.driver }, { "Physical Address", mac },
        { "DHCP Enabled", dhcp ? "Yes" : "No" }, { "IPv4 Address", netif.configured ? ip : "" }, { "IPv4 Subnet Mask", netif.configured ? mask : "" },
        { "Lease Obtained", lease }, { "Lease Expires", exp }, { "IPv4 Default Gateway", netif.configured && netif.gw ? gw : "" },
        { "IPv4 DHCP Server", dhcp && netif.dhcp_server ? srv : "" }, { "IPv4 DNS Server", netif.configured && netif.dns ? dns : "" },
        { "IPv4 WINS Server", "" }, { "NetBIOS over Tcpip Enabled", "No" },
    };
    int y = lr.y + 28;
    for (unsigned i = 0; i < ARRAY_LEN(rows) && y < lr.y + lr.h - 16; i++, y += 20) {
        draw_text_ellipsis(s, FONT_UI, lr.x + 8, y, cx - lr.x - 14, rows[i][0], T.text);
        draw_text_ellipsis(s, FONT_UI, cx + 6, y, lr.x + lr.w - cx - 12, rows[i][1], T.text);
    }
    if (ui_button_ex(w, R(s->w - 88, s->h - 36, 76, 24), "Close", true, true)) wm_destroy(w);
}
static void det_tick(window_t *w) { w->dirty = true; }
static void det_destroy(window_t *w) { (void)w; det_win = NULL; }
static void details_open(void) {
    if (det_win && wm_alive(det_win)) { wm_focus(det_win); return; }
    window_t *w = wm_create("Network Connection Details", -1, -1, 440, 360, WF_NO_MAX | WF_NO_MIN);
    w->paint = det_paint; w->destroy = det_destroy; w->icon = ICON_CP_NETWORK;
    wm_set_timer(w, 1000, det_tick);
    det_win = w; wm_center(w);
}

/* ---------------- Local Area Connection Status ---------------- */
static window_t *st_win;
static void row(canvas_t *s, int x, int y, int vx, const char *k, const char *v) { draw_text(s, FONT_UI, x, y, k, T.text); draw_text(s, FONT_UI, vx, y, v, T.text); }
static void st_paint(window_t *w) {
    canvas_t *s = w->surf;
    gfx_clear(s, T.face);
    ui_tab(w, R(6, 6, 70, 22), "General", true);
    rect_t body = R(6, 28, s->w - 12, s->h - 72);
    fill_rect(s, body.x, body.y, body.w, body.h, T.classic ? T.face : T.window_bg);
    draw_rect(s, body.x, body.y, body.w, body.h, T.face_dark);
    int x = 18, wd = s->w - 36, y = 40, vx = x + 160;
    bool up = netif.present && net_enabled() && netif.link;
    draw_group(s, R(x, y, wd, 146), "Connection");
    row(s, x + 12, y + 22, vx, "IPv4 Connectivity:", !up ? "No network access" : !netif.configured ? "Identifying..." : netif.gw ? "Internet" : "No Internet access");
    row(s, x + 12, y + 42, vx, "IPv6 Connectivity:", "No network access");
    row(s, x + 12, y + 62, vx, "Media State:", !net_enabled() ? "Disabled" : netif.link ? "Enabled" : "Media disconnected");
    char d[32]; duration_text(d, sizeof d);
    row(s, x + 12, y + 82, vx, "Duration:", up && netif.configured ? d : "");
    row(s, x + 12, y + 102, vx, "Speed:", up ? speed_text() : "");
    if (ui_button_ex(w, R(x + 12, y + 118, 90, 24), "Details...", up, false)) details_open();
    y += 158;
    draw_group(s, R(x, y, wd, 96), "Activity");
    int mid = x + wd / 2;
    draw_text(s, FONT_UI, mid - 90, y + 24, "Sent", T.text);
    draw_text(s, FONT_UI, mid + 90 - text_width(FONT_UI, "Received"), y + 24, "Received", T.text);
    hline(s, mid - 50, y + 31, 30, T.face_dark); hline(s, mid + 20, y + 31, 30, T.face_dark);
    adapter_icon(s, mid - 23, y + 14);
    char a[24], b[24];
    bytes_text(a, sizeof a, netif.tx_bytes); bytes_text(b, sizeof b, netif.rx_bytes);
    draw_text(s, FONT_UI, x + 12, y + 66, "Bytes:", T.text);
    draw_text(s, FONT_UI, mid - 20 - text_width(FONT_UI, a), y + 66, a, T.text);
    hline(s, mid, y + 60, 1, T.face_dark); for (int k = 0; k < 18; k++) fill_rect(s, mid, y + 60 + k, 1, 1, T.face_dark);
    draw_text(s, FONT_UI, x + wd - 14 - text_width(FONT_UI, b), y + 66, b, T.text);
    y += 108;
    int bw = (wd - 12) / 3;
    if (ui_button(w, R(x, y, bw, 24), "Properties")) props_open();
    if (ui_button(w, R(x + bw + 6, y, bw, 24), net_enabled() ? "Disable" : "Enable")) { set_enabled(!net_enabled()); w->dirty = true; }
    if (ui_button(w, R(x + 2 * (bw + 6), y, bw, 24), "Diagnose")) diagnose();
    if (ui_button_ex(w, R(s->w - 88, s->h - 36, 76, 24), "Close", true, true)) wm_destroy(w);
}
static void st_tick(window_t *w) { w->dirty = true; }
static void st_destroy(window_t *w) { (void)w; st_win = NULL; }
void netstatus_open(void) {
    if (st_win && wm_alive(st_win)) { wm_focus(st_win); return; }
    char t[96]; snprintf(t, sizeof t, "%s Status", conn_name());
    window_t *w = wm_create(t, -1, -1, 360, 400, WF_NO_MAX | WF_NO_MIN);
    w->paint = st_paint; w->destroy = st_destroy; w->icon = ICON_CP_NETWORK;
    wm_set_timer(w, 1000, st_tick);
    st_win = w; wm_center(w);
}

/* ---------------- the Network Connections window ---------------- */
typedef struct { bool sel, renaming; char name[64]; } nc_t;
static window_t *nc_win;
/* a command-bar button: flat text that lights up under the mouse */
static bool cmd(window_t *w, int *x, int y, const char *label, bool enabled) {
    canvas_t *s = w->surf;
    int tw = text_width(FONT_UI, label) + 18;
    rect_t r = R(*x, y + 3, tw, 24);
    *x += tw + 2;
    if (!enabled) { draw_text(s, FONT_UI, r.x + 9, r.y + 5, label, mix(T.text, HEX(0xE8EEF6), 55)); return false; }
    bool hot = ui_hover(w, r);
    if (hot) { fill_round_rect(s, r.x, r.y, r.w, r.h, 3, HEX(0xFFFFFF)); draw_round_rect(s, r.x, r.y, r.w, r.h, 3, HEX(0x9BB8DA)); }
    draw_text(s, FONT_UI, r.x + 9, r.y + 5, label, HEX(0x1E395B));
    return ui_click_area(w, r);
}
static void rename_done(nc_t *n) {
    n->renaming = false;
    if (n->name[0]) { settings_set("net_name", n->name); settings_save(); }
    char t[96];
    if (st_win && wm_alive(st_win)) { snprintf(t, sizeof t, "%s Status", conn_name()); wm_set_title(st_win, t); }
}
static void nc_paint(window_t *w) {
    nc_t *n = w->data; canvas_t *s = w->surf;
    gfx_clear(s, HEX(0xFFFFFF));
    /* address bar */
    gradient_v(s, 0, 0, s->w, 38, HEX(0xDCE8F5), HEX(0xC6D8EE));
    rect_t ab = R(10, 7, s->w - 20, 24);
    fill_rect(s, ab.x, ab.y, ab.w, ab.h, HEX(0xFFFFFF)); draw_rect(s, ab.x, ab.y, ab.w, ab.h, HEX(0x8DA6C4));
    draw_icon(s, ICON_CP_NETWORK, 16, ab.x + 4, ab.y + 4);
    int bx = ab.x + 26;
    static const char *crumbs[] = { "Control Panel", "Network and Internet", "Network Connections" };
    static const char *targets[] = { "", "cat:1", NULL };
    for (int i = 0; i < 3; i++) {
        draw_arrow(s, bx + 2, ab.y + 8, 3, HEX(0x5A6E86)); bx += 14;
        int tw = text_width(FONT_UI, crumbs[i]);
        rect_t cr = R(bx - 2, ab.y + 2, tw + 4, 20);
        if (targets[i] && ui_hover(w, cr)) draw_rect(s, cr.x, cr.y, cr.w, cr.h, HEX(0x9BB8DA));
        draw_text(s, FONT_UI, bx, ab.y + 5, crumbs[i], HEX(0x101820));
        if (targets[i] && ui_click_area(w, cr)) control_open(targets[i]);
        bx += tw + 8;
    }
    /* command bar */
    gradient_v3(s, 0, 38, s->w, 32, HEX(0xF5F9FD), HEX(0xE6EEF8), HEX(0xDCE6F4), 16);
    hline(s, 0, 69, s->w, HEX(0xA0B4CC));
    int cx = 8;
    bool has = netif.present;
    { int ox0 = cx; if (cmd(w, &cx, 38, "Organize   ", true)) {} draw_arrow(s, cx - 16, 38 + 13, 1, HEX(0x1E395B)); (void)ox0; }
    if (n->sel && has) {
        if (cmd(w, &cx, 38, net_enabled() ? "Disable this network device" : "Enable this network device", true)) set_enabled(!net_enabled());
        if (cmd(w, &cx, 38, "Diagnose this connection", true)) diagnose();
        if (cmd(w, &cx, 38, "Rename this connection", true)) { n->renaming = true; strlcpy(n->name, conn_name(), sizeof n->name); }
        if (cx + text_width(FONT_UI, "View status of this connection") + 20 < s->w && cmd(w, &cx, 38, "View status of this connection", true)) netstatus_open();
        if (cx + text_width(FONT_UI, "Change settings of this connection") + 20 < s->w && cmd(w, &cx, 38, "Change settings of this connection", true)) props_open();
    }
    /* the adapter tile */
    rect_t tr = R(14, 84, 300, 64);
    bool hot = ui_hover(w, tr);
    if (n->sel || hot) {
        fill_round_rect(s, tr.x, tr.y, tr.w, tr.h, 3, n->sel ? HEX(0xCCE8FF) : HEX(0xE5F3FF));
        draw_round_rect(s, tr.x, tr.y, tr.w, tr.h, 3, n->sel ? HEX(0x84ACDD) : HEX(0xB8D6FB));
    }
    if (!has) {
        draw_text_wrapped(s, FONT_UI, 20, 90, s->w - 40, "No network adapter was found. See Device Manager for the network hardware this computer has.", T.gray_text, 2);
        return;
    }
    adapter_icon(s, tr.x + 8, tr.y + 8);
    int tx = tr.x + 66;
    if (n->renaming) {
        bool foc = false;
        ui_textbox_ex(w, R(tx, tr.y + 6, tr.w - 74, 20), n->name, sizeof n->name, false, 0, &foc);
        if (!foc && !hot && w->ui.pressed) rename_done(n);
    } else draw_text_ellipsis(s, FONT_UI, tx, tr.y + 8, tr.w - 74, conn_name(), HEX(0x101820));
    draw_text_ellipsis(s, FONT_UI, tx, tr.y + 26, tr.w - 74, state_text(), HEX(0x6D6D6D));
    draw_text_ellipsis(s, FONT_UI, tx, tr.y + 42, tr.w - 74, netif.driver, HEX(0x6D6D6D));
    if (hot && w->ui.dblclick && !n->renaming) { n->sel = true; w->ui.dblclick = false; netstatus_open(); }
    if (ui_click_area(w, tr)) { n->sel = true; w->dirty = true; }
    else if (w->ui.pressed && !hot && ui_hover(w, R(0, 70, s->w, s->h - 70)) && !n->renaming) { n->sel = false; }
    /* the details pane at the bottom, like Explorer's */
    rect_t dp = R(0, s->h - 58, s->w, 58);
    gradient_v(s, dp.x, dp.y, dp.w, dp.h, HEX(0xF7FAFE), HEX(0xE3ECF7));
    hline(s, 0, dp.y, s->w, HEX(0xC4D2E3));
    if (n->sel) {
        adapter_icon(s, 12, dp.y + 6);
        draw_text(s, FONT_UI_BOLD, 76, dp.y + 8, conn_name(), HEX(0x101820));
        char l[160]; snprintf(l, sizeof l, "Status: %s    Device name: %s", state_text(), netif.driver);
        draw_text_ellipsis(s, FONT_UI, 76, dp.y + 28, s->w - 90, l, HEX(0x3C4B5C));
    } else draw_text(s, FONT_UI, 16, dp.y + 20, "1 item", HEX(0x3C4B5C));
}
static void nc_key(window_t *w, key_event_t *e) {
    nc_t *n = w->data;
    if (!e->pressed) return;
    if (n->renaming) { if (e->key == KEY_ENTER) rename_done(n); else if (e->key == KEY_ESC) n->renaming = false; w->dirty = true; return; }
    if (e->key == KEY_ENTER && n->sel) netstatus_open();
    else if (e->key == KEY_F2 && n->sel) { n->renaming = true; strlcpy(n->name, conn_name(), sizeof n->name); }
    else if (e->key == KEY_TAB || e->key == KEY_DOWN || e->key == KEY_UP || e->key == KEY_RIGHT || e->key == KEY_LEFT || e->key == KEY_HOME) n->sel = true;
    w->dirty = true;
}
static void nc_tick(window_t *w) { w->dirty = true; }
static void nc_destroy(window_t *w) { kfree(w->data); w->data = NULL; nc_win = NULL; }
void netconn_open(void) {
    if (nc_win && wm_alive(nc_win)) { wm_focus(nc_win); return; }
    nc_t *n = kzalloc(sizeof *n);
    window_t *w = wm_create("Network Connections", -1, -1, MIN(940, SCREEN_W - 40), MIN(420, SCREEN_H - 80), WF_RESIZABLE);
    w->data = n; w->paint = nc_paint; w->destroy = nc_destroy; w->key = nc_key; w->icon = ICON_CP_NETWORK;
    w->min_w = 420; w->min_h = 260;
    wm_set_timer(w, 1000, nc_tick);
    nc_win = w; wm_center(w);
}
