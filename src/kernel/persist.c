/* The user's settings and the desktop: copying C:\WRLD\wrld.ini's values to the variables
 * the desktop, the network and the sound use (settings_apply, after settings_load) and back
 * (settings_capture, before settings_save). Everything else about installed-system
 * persistence (the write-back of C: to the system disk, finding the system partition at
 * boot, the settings store and wrld.ini itself, the account password) is in Rust:
 * rust/src/kernel/persistk.rs and rust/src/persist.rs. */
#include "kernel.h"
#include "disk.h"
#include "gfx.h"
#include "theme.h"
#include "wm.h"
#include "audio.h"
#include "net.h"

void settings_apply(void);
void settings_capture(void);

/* account pictures are saved by name; the old four icon names map to the
 * first photograph so settings written before the change still load */
static int picture_from_name(const char *s, int def) {
    if (!s) return def;
    if (isdigit(s[0])) {
        int v = atoi(s);
        if (v >= 0 && v < user_picture_count) return v;
        if (v >= ICON_USER_FLOWER && v <= ICON_USER_MOUNTAIN) return 0;    /* older wrld.ini */
        return def;
    }
    if (!strcasecmp(s, "flower") || !strcasecmp(s, "planet") ||
        !strcasecmp(s, "fish") || !strcasecmp(s, "mountain")) return 0;
    if (!strcasecmp(s, "custom")) return user_picture_custom_exists() ? USER_PICTURE_CUSTOM : def;
    if (!strncasecmp(s, "avatar", 6)) {
        int v = atoi(s + 6) - 1;
        if (v >= 0 && v < user_picture_count) return v;
    }
    return def;
}
static const char *picture_name(int id) {
    static char b[16];
    if (id == USER_PICTURE_CUSTOM) return "custom";
    if (id < 0 || id >= user_picture_count) id = 0;
    snprintf(b, sizeof b, "avatar%d", id + 1);
    return b;
}
static int geti(const char *k, int def) { const char *v = settings_get(k); return v ? atoi(v) : def; }
void settings_apply(void) {
    const char *s;
    if ((s = settings_get("user")) && *s) strlcpy(user_name, s, sizeof user_name);
    if ((s = settings_get("computer")) && *s) strlcpy(computer_name, s, sizeof computer_name);
    strlcpy(user_password, settings_get("password") ? settings_get("password") : "", sizeof user_password);
    { extern bool adblock_enabled; adblock_enabled = geti("adblock", 1) != 0; }
    fw_enabled = geti("fw_on", 0) != 0; fw_block_tcp = geti("fw_tcp", 1) != 0; fw_block_udp = geti("fw_udp", 1) != 0;
    fw_clear_ports(); { const char *fp = settings_get("fw_ports"); while (fp && *fp) { char *e; unsigned long v = strtoul(fp, &e, 10); if (v) fw_allow_port((uint16_t)v); fp = (*e == 0) ? e : e + 1; } }
    user_picture = picture_from_name(settings_get("picture"), user_picture);
    tz_offset_minutes = geti("tz", tz_offset_minutes);
    mouse_speed = geti("mouse_speed", mouse_speed);
    clock_24h = geti("clock24", clock_24h);
    show_seconds = geti("seconds", show_seconds);
    audio_volume = CLAMP(geti("volume", audio_volume), 0, 100);
    system_sounds = geti("sounds", system_sounds);
    const char *rs = settings_get("resolution");
    if (rs && video_can_set_mode()) { int rw = atoi(rs); const char *xx = strchr(rs, 'x'); int rh = xx ? atoi(xx + 1) : 0; if (rw && rh) gfx_set_resolution(rw, rh); }
    const char *th = settings_get("theme");
    dark_mode = !boot_safe_mode && geti("dark", dark_mode ? 1 : 0) != 0;
    theme_overrides_load();
    cursor_set_load(settings_get("cursor"));
    if (!boot_safe_mode && th) {
        int id = isdigit(th[0]) ? atoi(th) : style_find(th);    /* numbers: older wrld.ini */
        theme_sat = CLAMP(geti("saturation", 0), -100, 100); theme_light = CLAMP(geti("lightness", 0), -60, 60);
        if (id >= 0 && id < THEME_COUNT) theme_apply(id, geti("hue", 0));
    } else if (!boot_safe_mode) theme_apply(theme_id, geti("hue", 0));
    wallpaper_fit = CLAMP(geti("wallpaper_fit", wallpaper_fit), 0, 2);
    sidebar_open = geti("sidebar", sidebar_open);
    sidebar_tiles = geti("side_tiles", sidebar_tiles);
    wm_animations = !boot_safe_mode && geti("animations", wm_animations ? 1 : 0) != 0;
    { extern bool accel3d; accel3d = !boot_safe_mode && geti("accel3d", accel3d ? 1 : 0) != 0; }
    wm_animation_style = CLAMP(geti("window_anim", wm_animation_style), WINDOW_ANIM_NONE, WINDOW_ANIM_COUNT - 1);
    startmenu_animation = CLAMP(geti("start_anim", startmenu_animation), MENU_ANIM_NONE, MENU_ANIM_COUNT - 1);
    sidebar_slide = geti("sidebar_slide", sidebar_slide ? 1 : 0) != 0;
    sidebar_tile_fade = geti("tile_fade", sidebar_tile_fade ? 1 : 0) != 0;
    wm_effects_changed();
    if ((s = settings_get("wallpaper"))) {
        /* saved by name; a number is an older setting, from before "Your picture" sat just
         * before "(None)": that number was "(None)" */
        if (isdigit((unsigned char)s[0])) { int v = atoi(s); wallpaper_id = v == wallpaper_count - 2 ? wallpaper_count - 1 : v; }
        else for (int i = 0; i < wallpaper_count; i++) if (!strcasecmp(wallpaper_names[i], s)) wallpaper_id = i;
        if (wallpaper_id < 0 || wallpaper_id >= wallpaper_count || (wallpaper_id == WALLPAPER_CUSTOM && !wallpaper_custom_exists())) wallpaper_id = 0;
        shell_wallpaper_changed();
    }
}
void settings_capture(void) {
    char b[16];
    settings_set("user", user_name); settings_set("computer", computer_name);
    settings_set("picture", picture_name(user_picture));
    settings_set("theme", style_file(theme_id));
    if (video_can_set_mode()) { snprintf(b, sizeof b, "%dx%d", SCREEN_W, SCREEN_H); settings_set("resolution", b); }
    snprintf(b, sizeof b, "%d", theme_hue); settings_set("hue", b);
    snprintf(b, sizeof b, "%d", theme_sat); settings_set("saturation", b);
    snprintf(b, sizeof b, "%d", theme_light); settings_set("lightness", b);
    settings_set("wallpaper", wallpaper_id >= 0 && wallpaper_id < wallpaper_count ? wallpaper_names[wallpaper_id] : "");
    snprintf(b, sizeof b, "%d", wallpaper_fit); settings_set("wallpaper_fit", b);
    snprintf(b, sizeof b, "%d", sidebar_open ? 1 : 0); settings_set("sidebar", b);
    snprintf(b, sizeof b, "%d", sidebar_tiles); settings_set("side_tiles", b);
    settings_set("animations", wm_animations ? "1" : "0");
    { extern bool accel3d; settings_set("accel3d", accel3d ? "1" : "0"); }
    snprintf(b, sizeof b, "%d", wm_animation_style); settings_set("window_anim", b);
    snprintf(b, sizeof b, "%d", startmenu_animation); settings_set("start_anim", b);
    settings_set("sidebar_slide", sidebar_slide ? "1" : "0"); settings_set("tile_fade", sidebar_tile_fade ? "1" : "0");
    snprintf(b, sizeof b, "%d", tz_offset_minutes); settings_set("tz", b);
    snprintf(b, sizeof b, "%d", mouse_speed); settings_set("mouse_speed", b);
    snprintf(b, sizeof b, "%d", clock_24h); settings_set("clock24", b);
    snprintf(b, sizeof b, "%d", show_seconds); settings_set("seconds", b);
    snprintf(b, sizeof b, "%d", audio_volume); settings_set("volume", b);
    snprintf(b, sizeof b, "%d", system_sounds); settings_set("sounds", b);
    snprintf(b, sizeof b, "%d", dark_mode ? 1 : 0); settings_set("dark", b);
    settings_set("cursor", cursor_set_active());
    settings_set("password", user_password);
    { extern bool adblock_enabled; settings_set("adblock", adblock_enabled ? "1" : "0"); }
    settings_set("fw_on", fw_enabled ? "1" : "0"); settings_set("fw_tcp", fw_block_tcp ? "1" : "0"); settings_set("fw_udp", fw_block_udp ? "1" : "0");
    { char fp[64] = ""; int o = 0; for (int i = 0; i < fw_port_count() && o < (int)sizeof fp - 8; i++) o += snprintf(fp + o, sizeof fp - o, "%s%u", i ? "," : "", fw_port_at(i)); settings_set("fw_ports", fp); }
}
