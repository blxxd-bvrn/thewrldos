/* The WRLD Xplorer - internal interfaces shared by html.c and browser.c */
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

typedef struct { char sel[48]; css_t css; } css_rule_t;
typedef struct {
    arena_t arena;
    node_t *root;
    char title[160];
    char base[1024];
    char refresh[1024]; int refresh_s;
    uint32_t bgcolor, text, link, vlink;
    css_rule_t *rules; int nrules;
} doc_t;

doc_t *html_parse(const char *src, uint32_t len, const char *charset, const char *url);
void doc_free(doc_t *d);
const char *node_attr(node_t *n, const char *name);
bool parse_color(const char *s, uint32_t *out);
int latin1_from_utf8(const uint8_t *s, uint32_t n, char *out);   /* returns length */

/* ad blocker (adblock.c) */
extern bool adblock_enabled;
bool adblock_blocks(const char *url, const char *page_url);     /* refuse this sub-resource? */
bool adblock_hides(const char *cls, const char *id);            /* hide this element? */
bool adblock_page_allowed(const char *page_url);                /* site on the allow list */
void adblock_reload(void);
#define ADBLOCK_LIST "C:\\WRLD\\adblock.txt"
#endif
