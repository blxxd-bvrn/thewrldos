#ifndef LOGON_H
#define LOGON_H
/* Logon skins: the look of the welcome (sign-in) screen.
 *
 * A skin is a .wrldlogon text file in LOGON_SKIN_DIR. It picks one of two
 * layouts and fills it in with colours, text and an optional picture:
 *   - "signin":  the sign-in screen (user list on the left, clock and
 *                Power Off / Help and Settings at the top right), the default;
 *   - "classic": the XP-style screen with the centre divider.
 * Logon Skin Maker writes these files; Logon Skin Themer picks one. */
#include "gfx.h"

#define LOGON_SKIN_DIR "C:\\WRLD\\Resources\\Logon Skins"
#define LOGON_SKIN_EXT "wrldlogon"
#define LOGON_DEFAULT_SKIN "Sign In"

enum { LOGON_LAYOUT_SIGNIN, LOGON_LAYOUT_CLASSIC, LOGON_LAYOUT_COUNT };

/* the colour roles, in file order; names are the keys in [Colors] */
enum {
    LC_BG_TOP, LC_BG_BOTTOM, LC_GLOW1, LC_GLOW2, LC_GLOW3, LC_ACCENT,
    LC_TEXT, LC_SUBTEXT, LC_PANEL_TOP, LC_PANEL_BOTTOM, LC_PANEL_TEXT,
    LC_BUTTON_TOP, LC_BUTTON_BOTTOM, LC_BUTTON_TEXT, LC_COUNT
};

typedef struct {
    char file[64];                  /* file name without extension */
    char name[48], desc[128];
    int layout;
    char title[48], subtitle[80], brand[32], image[160];
    uint32_t col[LC_COUNT];
    bool show_clock, show_guest, show_help, show_globe;
    bool builtin;                   /* shipped with the system: Themer will not delete it */
} logon_skin_t;

const char *logon_color_key(int role);                  /* "bg_top" ... */
const char *logon_color_label(int role, int layout);    /* NULL: the layout does not use it */
const char *logon_layout_name(int layout);              /* "Sign-in screen" / "Classic (XP)" */

void logon_skin_defaults(logon_skin_t *s, int layout);
void logon_skins_load(void);                            /* (re)scan LOGON_SKIN_DIR */
int logon_skin_count(void);
const logon_skin_t *logon_skin_get(int i);
int logon_skin_find(const char *name);                  /* file or display name; -1 if none */
int logon_skin_current_index(void);
const logon_skin_t *logon_skin_current(void);
void logon_skin_set_current(const char *file);          /* saved in the settings */
bool logon_skin_save(const logon_skin_t *s);            /* writes LOGON_SKIN_DIR\<file>.wrldlogon */
bool logon_skin_delete(const char *file);

/* Rendering. The screen and the previews in the two apps share this code. */
enum { HIT_NONE, HIT_USER, HIT_POWER, HIT_HELP, HIT_GUEST, HIT_SIGNIN, HIT_PW,
       HIT_NEXT_SKIN, HIT_HELP_CLOSE, HIT_COUNT };
typedef struct {
    int hot;                        /* HIT_* under the pointer */
    bool selected;                  /* sign-in: the user's panel is open */
    bool ask_pw;                    /* classic: the password box is showing */
    int pw_len;
    bool bad_pw;
    bool logging_in;
    int frame;
    bool help;                      /* sign-in: the Help and Settings panel */
    const char *notice;
    bool preview;                   /* no live clock: a fixed time */
    rect_t hit[HIT_COUNT];          /* filled in by logon_render; w = 0 when absent */
} logon_state_t;
void logon_render(canvas_t *c, const logon_skin_t *s, logon_state_t *st);
void logon_background(canvas_t *c, const logon_skin_t *s);   /* just the background */
/* render a skin at a virtual size and shrink it into r of dst (area-averaged) */
void logon_preview(canvas_t *dst, rect_t r, const logon_skin_t *s, int vw, int vh);
void logon_fullscreen_preview(const logon_skin_t *s);         /* until a click or key */
void canvas_shrink_into(canvas_t *dst, rect_t r, canvas_t *src);

void logonmaker_open(const char *skin);     /* Logon Skin Maker; NULL = a new skin */
void logonthemer_open(void);                /* Logon Skin Themer */
#endif
