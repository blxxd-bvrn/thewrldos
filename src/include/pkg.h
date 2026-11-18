/* Linux software packages (.deb and .rpm) on the 64-bit edition: reading them,
 * checking what they need, installing them into C:\Linux and removing them.
 * src/apps/pkg.c (the work and the dpkg/rpm/apt commands), src/apps/pkgui.c
 * (the installer window). */
#ifndef PKG_H
#define PKG_H
#include "kernel.h"

#define PKG_DB_DIR   "C:\\Linux\\var\\lib\\wrld\\packages"
#define PKG_BASE_LIST "C:\\Linux\\var\\lib\\wrld\\base-packages"

enum { PE_FILE, PE_DIR, PE_SYMLINK, PE_HARDLINK };
typedef struct {
    char path[256];              /* absolute Linux path, normalised ("/usr/bin/hello") */
    char link[256];              /* symlink target as written, or the hard link's archive path */
    const uint8_t *data;         /* into the unpacked payload */
    uint32_t size;
    uint8_t type;
} pkg_entry_t;

typedef struct {
    char fmt;                    /* 'd' (.deb) or 'r' (.rpm) */
    char file[256];              /* the package file (WRLD path) */
    char name[64], version[96], arch[24];
    char summary[160], maint[128], homepage[160], section[48], license[64];
    char *desc;                  /* long description */
    char *depends;               /* Depends + Pre-Depends (deb), or the requirements joined by ", " (rpm) */
    char *provides;
    uint32_t inst_kb;            /* installed size */
    char comp[8];                /* compression of the files */
    char scripts[96];            /* maintainer scripts present (not run) */
    char *postinst;              /* the post-install script, for its update-alternatives lines */
    uint8_t *payload; uint32_t payload_len;
    pkg_entry_t *ents; int nents, cap;
    int *pidx, *bidx; int icap;  /* hash indexes: path -> entry, base name -> entry */
    int nfiles;
    char err[200];
} pkg_t;

/* what a package needs that is not here */
typedef struct {
    char missing_pkgs[512];      /* unsatisfied Depends (deb) */
    char missing_libs[512];      /* shared libraries no file provides */
    char missing_vers[512];      /* library versions the installed libraries lack (GLIBC_2.40, ...) */
    char missing_interp[256];    /* script interpreters and ELF loaders that are not installed */
    char programs[512];          /* commands it adds (/usr/bin, /usr/games, ...) */
    bool foreign_elf;            /* 32-bit or non-x86 programs */
    bool ok;                     /* nothing missing */
} pkg_check_t;

typedef void (*pkg_out_fn)(void *ctx, const char *line);

bool pkg_load(const char *wrld_path, pkg_t *p);          /* false: p->err says why */
void pkg_free(pkg_t *p);
bool pkg_arch_ok(const pkg_t *p);                         /* amd64/x86_64/all/noarch */
void pkg_check(const pkg_t *p, pkg_check_t *c);
/* install / remove; progress lines go to out (may be NULL); *pct (may be NULL) follows the work */
bool pkg_install(pkg_t *p, pkg_out_fn out, void *ctx, volatile int *pct, char *msg, int mcap);
bool pkg_remove(const char *name, pkg_out_fn out, void *ctx, char *msg, int mcap);

/* the installed packages */
int pkg_installed_count(void);
bool pkg_installed_get(int i, char *name, int ncap, char *version, int vcap, char *summary, int scap, char *fmt);
bool pkg_is_installed(const char *name, char *version, int vcap);
int pkg_dependents(const char *name, char *out, int cap);  /* installed packages that depend on it */
extern int pkg_db_generation;                              /* changes when packages come and go */

/* programs with a menu entry (.desktop files) */
typedef struct { char name[64]; char exec[256]; char pkg[64]; bool terminal; } pkg_menu_t;
int pkg_menu_count(void);
const pkg_menu_t *pkg_menu_get(int i);
bool pkg_menu_launch(int i, char *err, int ecap);

/* the dpkg, rpm and apt commands */
int pkg_cli(int argc, char **argv, const char *cwd, pkg_out_fn out, void *ctx);

/* the installer window */
void pkg_installer_open(const char *wrld_path);
bool pkg_remove_confirm(const char *name);                 /* asks, then removes */

#endif
