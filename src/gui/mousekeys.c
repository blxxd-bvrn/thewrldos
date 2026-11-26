/* Mouse Keys, as in Windows' Ease of Access: the numeric keypad moves the pointer and clicks, for
 * a computer with no mouse (it turns itself on at start when no mouse is found) or for anyone who
 * prefers it. Left Alt + Left Shift + Num Lock turns it on and off; so does Control Panel > Ease
 * of Access. Saved as "mousekeys" (1 on, 0 off; absent: decided by whether a mouse is found).
 *
 *   8 2 4 6, 7 9 1 3   move (up, down, left, right and the diagonals); held keys speed up,
 *                      Ctrl moves four times as far
 *   5                  click        +  double-click
 *   /  -  *            the button 5 clicks with: left, right, both
 *   0 (Ins)            press and hold the button (to drag)       . (Del)  let go
 */
#include "kernel.h"
#include "wm.h"
#include "apps.h"
#include "disk.h"
#include "usb.h"

bool mouse_keys_on;
static int sel = MB_LEFT, held;
static int last_dir; static uint32_t last_t; static int accel;

static void set_on(bool on, bool tell) {
    mouse_keys_on = on;
    settings_set("mousekeys", on ? "1" : "0"); settings_save();
    if (tell) notify_balloon("Mouse Keys", on ? "Mouse Keys is on: the numeric keypad moves the pointer (8 2 4 6), 5 clicks, + double-clicks, 0 holds the button and . lets go."
                                               : "Mouse Keys is off.");
}
void mousekeys_set(bool on) { set_on(on, false); }

/* at start: the saved choice, or on when there is no mouse to use */
void mousekeys_boot_check(void) {
    const char *v = settings_get("mousekeys");
    if (v && *v) { mouse_keys_on = atoi(v) != 0; return; }
    if (!(ps2_detected() & 2) && !usb_has_mouse()) {
        mouse_keys_on = true;
        notify_balloon("No mouse found", "Mouse Keys is on: use the numeric keypad to move the pointer (8 2 4 6) and 5 to click. "
                       "Tab, the arrow keys, Enter and the Windows key work everywhere too.");
    }
}

static void click(int n) {
    for (int i = 0; i < n; i++) { mouse_keys_move(0, 0, held | sel); mouse_keys_move(0, 0, held); }
}

bool mousekeys_key(key_event_t *e) {
    if (e->pressed && e->key == KEY_NUMLOCK && (e->mods & MOD_ALT) && (e->mods & MOD_SHIFT)) { set_on(!mouse_keys_on, true); return true; }
    if (!mouse_keys_on || !(e->mods & MOD_KEYPAD) || (e->mods & MOD_ALT)) return false;
    int dx = 0, dy = 0;
    switch (e->key) {
    case KEY_UP: dy = -1; break;           case KEY_DOWN: dy = 1; break;
    case KEY_LEFT: dx = -1; break;         case KEY_RIGHT: dx = 1; break;
    case KEY_HOME: dx = -1; dy = -1; break; case KEY_PGUP: dx = 1; dy = -1; break;
    case KEY_END: dx = -1; dy = 1; break;  case KEY_PGDN: dx = 1; dy = 1; break;
    case '5': if (e->pressed) click(1); return true;
    case '+': if (e->pressed) click(2); return true;
    case '/': if (e->pressed) sel = MB_LEFT; return true;
    case '-': if (e->pressed) sel = MB_RIGHT; return true;
    case '*': if (e->pressed) sel = MB_LEFT | MB_RIGHT; return true;
    case KEY_INSERT: if (e->pressed) { held |= sel; mouse_keys_move(0, 0, held); } return true;
    case KEY_DELETE: if (e->pressed) { held = 0; mouse_keys_move(0, 0, 0); } return true;
    default: return false;
    }
    if (!e->pressed) return true;
    /* a key held down repeats: each repeat in the same direction goes a little further */
    int dir = (dy + 1) * 3 + dx + 1;
    uint32_t now = timer_ticks();
    accel = dir == last_dir && now - last_t < 250 ? MIN(accel + 1, 20) : 0;
    last_dir = dir; last_t = now;
    int step = 8 + accel * 3;                              /* 8 pixels a press, up to 68 when held */
    if (e->mods & MOD_CTRL) step *= 4;
    mouse_keys_move(dx * step, dy * step, held);
    return true;
}
