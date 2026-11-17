/* Writes an XKB keymap (evdev rules, pc105, US layout) as text, for wrldcomp
 * to hand to Wayland clients: compiled once here, so THE WRLD OS needs no XKB
 * data files.   gen_keymap > keymap.xkb */
#include <stdio.h>
#include <stdlib.h>
#include <xkbcommon/xkbcommon.h>
int main(void) {
    struct xkb_context *ctx = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
    struct xkb_rule_names names = { "evdev", "pc105", "us", "", "" };
    struct xkb_keymap *km = ctx ? xkb_keymap_new_from_names(ctx, &names, XKB_KEYMAP_COMPILE_NO_FLAGS) : NULL;
    if (!km) { fprintf(stderr, "gen_keymap: cannot compile the keymap (is xkb-data installed?)\n"); return 1; }
    char *s = xkb_keymap_get_as_string(km, XKB_KEYMAP_FORMAT_TEXT_V1);
    fputs(s, stdout);
    free(s); xkb_keymap_unref(km); xkb_context_unref(ctx);
    return 0;
}
