/* Application registry, file associations and command execution */
#include "kernel.h"
#include "wm.h"
#include "apps.h"
#include "pkg.h"
#include "mediadec.h"
#include "theme.h"
#include "audio.h"
#include "logon.h"

const app_info_t app_table[] = {
    { "xplorer", "The WRLD Browser", "Browse the web", ICON_BROWSER },
    { "explorer", "File Explorer", "Browse files and folders", ICON_EXPLORER },
    { "notepad", "Notepad", "Edit text files", ICON_NOTEPAD },
    { "cmd", "Command Prompt", "Shell & scripts", ICON_TERMINAL },
    { "calc", "Calculator", "Perform calculations", ICON_CALCULATOR },
    { "paint", "Paint", "Draw pictures", ICON_PAINT },
    { "taskmgr", "Task Manager", "Processes and performance", ICON_TASKMGR },
    { "control", "Control Panel", "Change settings", ICON_CONTROL_PANEL },
    { "snake", "Snake", "A small arcade game", ICON_GAME },
    { "minesweeper", "Minesweeper", "Clear the minefield", ICON_GAME },
    { "pong", "Pong", "Two-paddle arcade game", ICON_GAME },
    { "wmplayer", "THE WRLD Media Player", "Play music and sounds", ICON_MEDIA_PLAYER },
    { "wrld3d", "WRLD 3D", "Test the 3D engine", ICON_CP_DISPLAY },
    { "logonthemer", "Logon Skin Themer", "Choose the look of the welcome screen", ICON_CP_APPEARANCE },
    { "logonmaker", "Logon Skin Maker", "Make your own welcome screen skin", ICON_CP_APPEARANCE },
    { "security", "WRLD Security Center", "Firewall, password, web protection and system health", ICON_CP_SYSTEM },
    { "about", "About " OS_NAME, "Version information", ICON_ABOUT },
    { "help", "Help and Support", "Read the manual", ICON_HELP },
    { "setup", "Install " OS_NAME, "Install to a hard disk", ICON_SETUP },
};
const int app_count = ARRAY_LEN(app_table);
const app_info_t *app_find(const char *id) {
    if (!strcasecmp(id, "xplorerlite") || !strcasecmp(id, "wrldbrowser") || !strcasecmp(id, "browser")) id = "xplorer";
    for (int i = 0; i < app_count; i++) if (!strcasecmp(app_table[i].id, id)) return &app_table[i]; return NULL; }

void app_launch(const char *name, const char *arg) {
    if (!strcasecmp(name, "explorer")) explorer_open(arg ? arg : USER_DOCS);
    else if (!strcasecmp(name, "notepad")) notepad_open(arg);
    else if (!strcasecmp(name, "xplorerlite") || !strcasecmp(name, "classicbrowser")) browser_classic_open(arg);   /* the built-in engine */
    else if (!strcasecmp(name, "xplorer") || !strcasecmp(name, "wrldbrowser") || !strcasecmp(name, "browser") || !strcasecmp(name, "iexplore")) browser_open(arg);
    else if (!strcasecmp(name, "cmd") || !strcasecmp(name, "terminal") || !strcasecmp(name, "sh") || !strcasecmp(name, "bash")) terminal_open(arg, NULL);
    else if (!strcasecmp(name, "calc")) calc_open();
    else if (!strcasecmp(name, "paint") || !strcasecmp(name, "mspaint")) paint_open(arg);
    else if (!strcasecmp(name, "taskmgr")) taskmgr_open();
    else if (!strcasecmp(name, "control")) control_open(arg);
    else if (!strcasecmp(name, "snake")) snake_open();
    else if (!strcasecmp(name, "minesweeper") || !strcasecmp(name, "mine")) minesweeper_open();
    else if (!strcasecmp(name, "pong")) pong_open();
    else if (!strcasecmp(name, "wmplayer") || !strcasecmp(name, "mediaplayer") || !strcasecmp(name, "wmplayer.exe")) mediaplayer_open(arg);
    else if (!strcasecmp(name, "wrld3d") || !strcasecmp(name, "3d")) demo3d_open();
    else if (!strcasecmp(name, "logonthemer") || !strcasecmp(name, "logonskins")) logonthemer_open();
    else if (!strcasecmp(name, "logonmaker")) logonmaker_open(arg);
    else if (!strcasecmp(name, "security") || !strcasecmp(name, "firewall")) security_open();
    else if (!strcasecmp(name, "about") || !strcasecmp(name, "winver")) about_open();
    else if (!strcasecmp(name, "help")) notepad_open("C:\\WRLD\\Help.txt");
    else if (!strcasecmp(name, "run")) shell_show_run();
    else if (!strcasecmp(name, "setup")) { extern void setup_launch_from_desktop(void); setup_launch_from_desktop(); }
    else msgbox("Error", "Unknown application.", MBI_ERROR, MBX_OK, NULL, NULL);
}

int file_icon(vnode_t *n) {
    if (n->dir) {
        if (str_ends_with_ci(n->name, ".app")) return ICON_APP_BUNDLE;
        if (!n->parent) return vfs_drive_kind(n->drive) == 2 ? ICON_DRIVE_DMG : ICON_DRIVE;
        if (!strcasecmp(n->name, "My Documents")) return ICON_MY_DOCUMENTS;
        return ICON_FOLDER;
    }
    const char *e = vfs_ext(n->name);
    if (!strcasecmp(e, "txt") || !strcasecmp(e, "log") || !strcasecmp(e, "ini") || !strcasecmp(e, "md") || !strcasecmp(e, "cfg")) return ICON_TEXT_FILE;
    if (!strcasecmp(e, "c") || !strcasecmp(e, "h") || !strcasecmp(e, "plist") || !strcasecmp(e, "xml") || !strcasecmp(e, "json")) return ICON_NOTEPAD;
    if (!strcasecmp(e, "exe") || !strcasecmp(e, "com")) return ICON_EXE;
    if (!strcasecmp(e, "sh") || !strcasecmp(e, "bash") || !strcasecmp(e, "bat") || !strcasecmp(e, "cmd")) return ICON_SCRIPT;
    if (!strcasecmp(e, "dmg") || !strcasecmp(e, "img") || !strcasecmp(e, "iso")) return ICON_DMG;
    if (!strcasecmp(e, "deb") || !strcasecmp(e, "rpm")) return ICON_PACKAGE;
    if (!strcasecmp(e, "wrldstyle") || !strcasecmp(e, "wrldlogon")) return ICON_CP_APPEARANCE;
    if (!strcasecmp(e, "wav") || !strcasecmp(e, "mp3") || !strcasecmp(e, "wma") || !strcasecmp(e, "m3u")) return ICON_MUSIC_FILE;
    if (!strcasecmp(e, "bmp") || !strcasecmp(e, "png") || !strcasecmp(e, "jpg") || !strcasecmp(e, "jpeg") || !strcasecmp(e, "gif")) return ICON_IMAGE_FILE;
    if (!strcasecmp(e, "html") || !strcasecmp(e, "htm") || !strcasecmp(e, "url")) return ICON_BROWSER;
    if (!strcasecmp(e, "dll") || !strcasecmp(e, "sys")) return ICON_SYS_FILE;
    if (!strcasecmp(e, "lnk")) {
        /* shortcut: icon of the target app */
        if (n->data && n->size) { char t[64]; int l = MIN(n->size, 63u); memcpy(t, n->data, l); t[l] = 0; char *nl = strchr(t, '\n'); if (nl) *nl = 0; char *r = strchr(t, '\r'); if (r) *r = 0; const app_info_t *a = app_find(t); if (a) return a->icon; vnode_t *tn = vfs_resolve(t, NULL); if (tn && tn != n) return file_icon(tn); }
        return ICON_EXE;
    }
    return ICON_FILE;
}
const char *file_type_name(vnode_t *n) {
    if (n->dir) return !n->parent ? "Local Disk" : str_ends_with_ci(n->name, ".app") ? "Application Bundle" : "File Folder";
    const char *e = vfs_ext(n->name);
    if (!strcasecmp(e, "txt")) return "Text Document";
    if (!strcasecmp(e, "exe")) return "Application";
    if (!strcasecmp(e, "sh")) return "Shell Script";
    if (!strcasecmp(e, "dmg")) return "Apple Disk Image";
    if (!strcasecmp(e, "deb")) return "Debian Software Package";
    if (!strcasecmp(e, "rpm")) return "RPM Software Package";
    if (!strcasecmp(e, "bmp")) return "Bitmap Image";
    if (!strcasecmp(e, "dll")) return "Application Extension";
    if (!strcasecmp(e, "lnk")) return "Shortcut";
    if (!strcasecmp(e, "html") || !strcasecmp(e, "htm")) return "HTML Document";
    if (!strcasecmp(e, "url")) return "Internet Shortcut";
    if (!strcasecmp(e, "wrldstyle")) return "Visual Style";
    if (!strcasecmp(e, "wrldlogon")) return "Logon Skin";
    if (!strcasecmp(e, "wav")) return "Wave Sound";
    if (!strcasecmp(e, "mp3")) return "MP3 Audio File";
    if (!strcasecmp(e, "mp2") || !strcasecmp(e, "mp1") || !strcasecmp(e, "mpa")) return "MPEG Audio File";
    if (!strcasecmp(e, "ogg") || !strcasecmp(e, "oga")) return "Ogg Vorbis Audio";
    if (!strcasecmp(e, "flac") || !strcasecmp(e, "fla")) return "FLAC Audio";
    if (!strcasecmp(e, "aif") || !strcasecmp(e, "aiff") || !strcasecmp(e, "aifc")) return "AIFF Sound";
    if (!strcasecmp(e, "au") || !strcasecmp(e, "snd")) return "AU Sound";
    if (!strcasecmp(e, "mpg") || !strcasecmp(e, "mpeg") || !strcasecmp(e, "m1v") || !strcasecmp(e, "mpe")) return "Movie File (MPEG)";
    if (!strcasecmp(e, "plist")) return "Property List";
    if (!strcasecmp(e, "c")) return "C Source File";
    return "File";
}

static void err_box(const char *title, const char *msg) { msgbox(title, msg, MBI_ERROR, MBX_OK, NULL, NULL); }

bool shell_open(const char *path, const char *cwd) {
    if (!strcmp(path, "::computer") || !strcmp(path, "::recycle")) { explorer_open(path); return true; }
    vnode_t *n = vfs_resolve(path, cwd);
    if (!n) return false;
    char full[512]; vfs_path(n, full, sizeof full);
    if (n->dir) {
        if (str_ends_with_ci(n->name, ".app")) bundle_open(n);
        else explorer_open(full);
        return true;
    }
    const char *e = vfs_ext(n->name);
    if (!strcasecmp(e, "exe") || !strcasecmp(e, "com")) {
        char err[256];
        extern bool pe_is_gui(vnode_t *n);
        if (!pe_probe(n, err, sizeof err)) err_box(n->name, err);
        else if (pe_is_gui(n)) { if (pe_spawn(full, "", NULL, err, sizeof err) < 0) err_box(n->name, err); }
        else { char dir[512]; path_dirname(full, dir, sizeof dir); terminal_open(full, dir); }
    } else if (!strcasecmp(e, "sh") || !strcasecmp(e, "bash")) {
        char dir[512]; path_dirname(full, dir, sizeof dir);
        terminal_open(full, dir);
    } else if (!strcasecmp(e, "dmg") || !strcasecmp(e, "img")) {
        char msg[400];
        char l = dmg_mount(n, msg, sizeof msg);
        if (!l) err_box("Disk Image", msg);
        else { char root[4] = { l, ':', '\\', 0 }; notify_balloon("Disk image mounted", msg); explorer_open(root); }
    } else if (!strcasecmp(e, "deb") || !strcasecmp(e, "rpm")) {
        pkg_installer_open(full);
    } else if (mdec_known_ext(e)) {
        mediaplayer_open(full);
    } else if (!strcasecmp(e, "wrldlogon")) {
        /* install it into the skin folder if it is elsewhere, then edit it */
        char dir[512]; path_dirname(full, dir, sizeof dir);
        if (strcasecmp(dir, LOGON_SKIN_DIR)) { vnode_t *d = vfs_mkdir_p(LOGON_SKIN_DIR); if (d) vfs_copy(n, d, NULL); }
        logon_skins_load();
        logonmaker_open(n->name);
    } else if (!strcasecmp(e, "wrldstyle")) {
        /* apply the visual style, then show it in Personalization */
        char dir[512]; path_dirname(full, dir, sizeof dir);
        if (strcasecmp(dir, STYLE_DIR)) {   /* install it into the Themes folder */
            char dst[512]; path_join(dst, sizeof dst, STYLE_DIR, n->name);
            vfs_mkdir_p(STYLE_DIR);
            vfs_write_file(dst, n->data, n->size);
        }
        styles_load();
        int id = style_find(n->name);
        if (id < 0) err_box("Visual Style", "This visual style could not be loaded.");
        else { theme_apply(id, 0); wm_redraw_all(); control_open("personalization"); }
    } else if (!strcasecmp(e, "html") || !strcasecmp(e, "htm") || !strcasecmp(e, "url")) {
        char u[700];
        if (!strcasecmp(e, "url")) { int l = MIN(n->size, 600u); memcpy(u, n->data, l); u[l] = 0; char *nl = strpbrk(u, "\r\n"); if (nl) *nl = 0; char *eq = strstr(u, "URL="); if (eq) memmove(u, eq + 4, strlen(eq + 4) + 1); }
        else { snprintf(u, sizeof u, "file:///%s", full); for (char *q = u; *q; q++) if (*q == '\\') *q = '/'; }
        browser_open(u);
    } else if (!strcasecmp(e, "bmp") || !strcasecmp(e, "png") || !strcasecmp(e, "jpg") || !strcasecmp(e, "jpeg") || !strcasecmp(e, "gif")) imageview_open(full);
    else if (!strcasecmp(e, "lnk")) {
        char t[256]; int l = MIN(n->size, 255u); memcpy(t, n->data, l); t[l] = 0;
        char *nl = strchr(t, '\n'); if (nl) *nl = 0; char *r = strchr(t, '\r'); if (r) *r = 0;
        return shell_execute(t, NULL);
    } else {
        /* Mach-O and unknown binaries */
        char desc[200];
        if (macho_probe(n->data, n->size, desc, sizeof desc)) {
            char m[400]; snprintf(m, sizeof m, "'%s' is a macOS program (%s).\n\nNative macOS binaries cannot run on " OS_NAME ". Script-based .app bundles and shell scripts can.", n->name, desc);
            err_box("Cannot run program", m);
        } else if (n->size >= 4 && !memcmp(n->data, "\x7F" "ELF", 4)) {
            if (ARCH_BITS == 64) {                                  /* a Linux program: in a Command Prompt */
                char dir[512], line[600]; path_dirname(full, dir, sizeof dir); snprintf(line, sizeof line, "\"%s\"", full);
                terminal_run(line, dir, n->name);
            } else err_box("Cannot run program", "This is a Linux program. Linux programs run on the 64-bit edition of " OS_NAME ".");
        } else notepad_open(full);
    }
    return true;
}

bool shell_execute(const char *cmdline, const char *cwd) {
    char buf[256]; strlcpy(buf, cmdline, sizeof buf);
    char *s = buf; while (*s == ' ') s++;
    if (!*s) return false;
    char *arg = NULL;
    if (*s == '"') { s++; char *e = strchr(s, '"'); if (e) { *e = 0; arg = e + 1; } }
    else { char *sp = strchr(s, ' '); if (sp) { *sp = 0; arg = sp + 1; } }
    if (arg) { while (*arg == ' ') arg++; if (!*arg) arg = NULL; }
    if (app_find(s) || !strcasecmp(s, "terminal") || !strcasecmp(s, "mspaint") || !strcasecmp(s, "winver") || !strcasecmp(s, "sh") || !strcasecmp(s, "bash")) { app_launch(s, arg); return true; }
    if (!strcasecmp(s, "notepad.exe")) { app_launch("notepad", arg); return true; }
    if (!strcasecmp(s, "cmd.exe")) { app_launch("cmd", arg); return true; }
    if (!strcasecmp(s, "calc.exe")) { app_launch("calc", NULL); return true; }
    if (!strcasecmp(s, "explorer.exe")) { app_launch("explorer", arg); return true; }
    if (!strcasecmp(s, "control.exe")) { app_launch("control", arg); return true; }
    if (!strcasecmp(s, "taskmgr.exe")) { app_launch("taskmgr", NULL); return true; }
    vnode_t *n = vfs_resolve(s, cwd);
    if (!n) {
        /* search PATH-like locations */
        const char *dirs[] = { "C:\\WRLD\\System32", "C:\\WRLD", "C:\\Samples", "C:\\Program Files" };
        char p[300];
        for (unsigned i = 0; i < ARRAY_LEN(dirs) && !n; i++) {
            snprintf(p, sizeof p, "%s\\%s", dirs[i], s); n = vfs_resolve(p, NULL);
            if (!n) { snprintf(p, sizeof p, "%s\\%s.exe", dirs[i], s); n = vfs_resolve(p, NULL); }
        }
    }
    if (!n) return false;
    char full[512]; vfs_path(n, full, sizeof full);
    if (!n->dir && !strcasecmp(vfs_ext(n->name), "exe") && arg) {
        char err[256]; if (pe_spawn(full, arg, NULL, err, sizeof err) < 0) err_box(n->name, err);
        return true;
    }
    return shell_open(full, NULL);
}

bool macho_probe(const uint8_t *d, uint32_t n, char *desc, int cap) {
    if (n < 8) return false;
    uint32_t m = *(const uint32_t *)d;
    const char *arch = NULL;
    if (m == 0xFEEDFACE || m == 0xFEEDFACF) {
        uint32_t cpu = ((const uint32_t *)d)[1];
        arch = cpu == 7 ? "i386" : cpu == 0x01000007 ? "x86_64" : cpu == 0x0100000C ? "arm64" : cpu == 12 ? "arm" : "unknown CPU";
        snprintf(desc, cap, "Mach-O %s executable", arch);
        return true;
    }
    if (m == 0xBEBAFECA) { snprintf(desc, cap, "Mach-O universal binary"); return true; }
    return false;
}
