/* XplorerLite - internal interfaces shared by html.c and browser.c */
#ifndef BROWSER_H
#define BROWSER_H
#include "kernel.h"
#include "gfx.h"

/* page memory: everything for one page comes from an arena that is freed at once */
typedef struct arena_chunk { struct arena_chunk *next; uint32_t used, cap; uint8_t data[]; } arena_chunk_t;
typedef struct { arena_chunk_t *head; } arena_t;
void *arena_alloc(arena_t *a, uint32_t n);
char *arena_strdup(arena_t *a, const char *s, int n);
void arena_free(arena_t *a);

/* CSS-lite: the properties the layout engine understands */
#define ST_COLOR    0x001
#define ST_BG       0x002
#define ST_DISPLAY  0x004
#define ST_ALIGN    0x008
#define ST_BOLD     0x010
#define ST_ITALIC   0x020
#define ST_UNDER    0x040
#define ST_SIZE     0x080
typedef struct { uint16_t set; uint32_t color, bg; int8_t display; /* 0 none, 1 block, 2 inline */ int8_t align; bool bold, italic, under; int8_t size; } css_t;

typedef struct node {
    uint8_t type;                 /* 0 element, 1 text */
    char tag[12];
    char *text;                   /* text nodes (Latin-1, entities decoded) */
    char **attr; int nattr;       /* name, value pairs */
    css_t css;                    /* from <style> rules and style="" */
    int aux;                      /* image or form-control index + 1 */
    struct node *parent, *first, *last, *next;
} node_t;

/* one complex selector of a style rule (selectors are parsed by Lexbor, see lexglue.h) */
typedef struct { void *sel; uint32_t spec, order; css_t css; } css_rule_t;
typedef struct css_bucket { uint32_t hash; int kind; char *key; int *idx; int n, cap; struct css_bucket *next; } css_bucket_t;
#define CSS_BUCKETS 256
#define MAX_LINKED_SHEETS 12
typedef struct {
    arena_t arena;
    node_t *root;                 /* stands in for <body>, whose attributes it carries */
    char title[160];
    char base[1024];
    char refresh[1024]; int refresh_s;
    uint32_t bgcolor, text, link, vlink;
    css_rule_t *rules; int nrules, caprules;
    css_bucket_t *buckets[CSS_BUCKETS];
    void *selpool;
    int nsheets;                  /* <style> and <link> sheets seen, in document order */
    struct { char *href; int sheet; } links[MAX_LINKED_SHEETS]; int nlinks;
} doc_t;

doc_t *html_parse(const char *src, uint32_t len, const char *charset, const char *url);
doc_t *html_parse_fragment(doc_t *page, const char *context_tag, const char *utf8, const char *url);
void doc_free(doc_t *d);
const char *node_attr(node_t *n, const char *name);
bool parse_color(const char *s, uint32_t *out);
int latin1_from_utf8(const uint8_t *s, uint32_t n, char *out);   /* returns length */


/* ---------------- page model (shared by browser.c and dom.c) ---------------- */
enum { IT_TEXT, IT_RECT, IT_IMG, IT_BULLET, IT_FRAME, IT_CTRL, IT_HLINE };
typedef struct { uint8_t kind; uint8_t under; int16_t font; int x, y, w, h, asc; uint32_t color; const char *text; int len; int link, idx; node_t *node; } item_t;
typedef struct { char *url; canvas_t *img, *scaled; int state; /* 0 pending, 1 ok, 2 failed */ char *alt; } pimg_t;
enum { C_TEXT, C_PASSWORD, C_SUBMIT, C_BUTTON, C_CHECK, C_RADIO, C_HIDDEN, C_SELECT, C_TEXTAREA, C_RESET };
typedef struct { int type, form; node_t *node; char name[64]; char value[512]; char label[64]; bool checked; int sel, nopts; char **opts, **optvals; rect_t r; } ctrl_t;
typedef struct { char action[1024]; bool post; node_t *node; } form_t;
/* events the window hands to the page's script task */
typedef struct { node_t *node; char type[16]; int key; int link; int ctrl; } page_event_t;
#define PAGE_EVQ 32
struct jspage;
typedef struct page {
    doc_t *doc;
    char url[1024], title[160], referrer[1024];
    bool secure; char tls[80];
    char *source; uint32_t srclen;
    pimg_t *imgs; int nimgs;
    ctrl_t *ctrls; int nctrls;
    form_t *forms; int nforms;
    item_t *items; int nitems, capitems;
    char **links; int nlinks, caplinks;
    struct { const char *name; int y; } *anchors; int nanchors, capanchors;
    int height, lay_width;
    uint32_t bg, fg, linkc;
    bool worker_running, orphan;
    bool ads_ok; int blocked;                 /* ad blocker: site allowed / items removed */
    /* scripting */
    struct jspage *js;
    void *browser;                            /* browser_t that shows this page */
    bool tree_dirty, title_dirty, scripts_pending;
    page_event_t evq[PAGE_EVQ]; int evq_head, evq_tail;
    uint64_t mem_limit;                       /* next memory warning (bytes) */
    int mem_answer;                           /* 0 none, -1 waiting, 1 continue, 2 stop */
    int scripts_run, script_errors, alerts;
    int gen;                                  /* browser generation that loaded this page */
} page_t;

/* browser.c services used by dom.c */
extern bool js_enabled;
void page_rebuild(page_t *p);
uint64_t page_memory(page_t *p);
void html_restyle(doc_t *d, node_t *n);                 /* html.c: re-apply CSS to n and its subtree */
void html_restyle_all(doc_t *d);
void html_add_stylesheet(doc_t *d, const char *css, uint32_t len, int sheet);   /* a downloaded <link> sheet */
int html_matches(node_t *n, const char *sel_utf8);      /* 1 match, 0 no, -1 invalid selector */
int html_selector_valid(const char *sel_utf8);
const char *html_parser_version(void);
void css_parse_decls(const char *s, int n, css_t *out); /* html.c */
bool html_is_void(const char *tag);

/* ad blocker (adblock.c) */
extern bool adblock_enabled;
bool adblock_blocks(const char *url, const char *page_url);     /* refuse this sub-resource? */
bool adblock_hides(const char *cls, const char *id);            /* hide this element? */
bool adblock_page_allowed(const char *page_url);                /* site on the allow list */
void adblock_reload(void);
#define ADBLOCK_LIST "C:\\WRLD\\adblock.txt"
#endif
