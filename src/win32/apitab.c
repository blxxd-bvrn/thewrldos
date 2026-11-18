/* The newer API tables (registry.c, k32more.c, shell.c, comctl.c, user32more): looked up
 * by DLL group before the older export tables in pe.c, so a fuller implementation here
 * replaces a stub there. */
#include "w32.h"

extern const w32_api_t registry_api[], k32more_api[], shell_api[], comctl_api[], usermore_api[], misc_api[];
static const w32_api_t *const tables[] = { registry_api, k32more_api, shell_api, comctl_api, usermore_api, misc_api };

void *w32_extra_find(const char *dll, const char *name) {
    char g[64]; w32_dll_group(dll ? dll : "", g, sizeof g);
    if (!strcmp(g, "kernelbase") || !strcmp(g, "ntdll")) strlcpy(g, "kernel32", sizeof g);
    for (unsigned t = 0; t < ARRAY_LEN(tables); t++)
        for (const w32_api_t *e = tables[t]; e->name; e++)
            if ((!dll || !strcmp(e->group, g)) && !strcmp(e->name, name)) return e->fn;
    return NULL;
}
