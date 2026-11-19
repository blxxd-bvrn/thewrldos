/* dinput8.dll and dinput.dll: DirectInput's system keyboard and mouse for Win32 programs.
 *
 * DirectInput8Create gives IDirectInput8A/W; DirectInputCreateA/W/Ex give IDirectInput7A/W,
 * whose first methods are those of IDirectInputA and IDirectInput2A, so one implementation
 * serves every version. Devices have IDirectInputDevice8A's method table (a superset of the
 * older device interfaces). The keyboard reports DIK scan codes from the key state user32
 * keeps for the program's windows; the mouse reports relative motion, the wheel and three
 * buttons. Immediate data (GetDeviceState) and buffered data (GetDeviceData, from the
 * differences between polls) both work, in the standard formats (c_dfDIKeyboard,
 * c_dfDIMouse, c_dfDIMouse2) and in a program's own DIDATAFORMAT. There are no joysticks
 * or force feedback. Method tables: tools/gen_com.py from mingw-w64's dinput.h. */
#include "w32.h"

#define P w32_cur()
typedef int32_t HRESULT;
#define DI_OK 0
#define DI_BUFFEROVERFLOW 1
#define DIERR_INVALIDPARAM ((HRESULT)0x80070057)
#define DIERR_NOINTERFACE ((HRESULT)0x80004002)
#define DIERR_UNSUPPORTED ((HRESULT)0x80004001)
#define DIERR_DEVICENOTREG ((HRESULT)0x80040154)
#define DIERR_NOTACQUIRED ((HRESULT)0x8007000C)
#define DIERR_OBJECTNOTFOUND ((HRESULT)0x80070002)

bool w32_vk_down(int vk);
int w32_wheel_take(void);
bool w32_has_focus(w32_proc_t *p);

/* ---------------- method tables ---------------- */
enum { IF_DI8, IF_DI7, IF_DEV, IF_COUNT };
typedef struct { int iface, index, nargs; const char *name; } method_t;
static const method_t methods[] = {
#define IDirectInput8A IF_DI8
#define IDirectInput7A IF_DI7
#define IDirectInputDevice8A IF_DEV
#define IDirectInputA -1
#define IDirectInputDevice7A -1
#define M(i, idx, name, n) { i, idx, n, #name },
#include "dinput_methods.h"
#undef M
};
typedef uintptr_t A;
#define STUB(n, ...) static HRESULT WINAPI stub##n(void *self, ##__VA_ARGS__) { (void)self; return DIERR_UNSUPPORTED; }
STUB(0) STUB(1, A a) STUB(2, A a, A b) STUB(3, A a, A b, A c) STUB(4, A a, A b, A c, A d) STUB(5, A a, A b, A c, A d, A e)
STUB(6, A a, A b, A c, A d, A e, A f)
static void *const stubs[] = { stub0, stub1, stub2, stub3, stub4, stub5, stub6 };
typedef struct { int iface; const char *name; void *fn; } impl_t;
static void *vtables[IF_COUNT][40];
static bool vt_ready;
static void build_vtables(void);

/* ---------------- objects ---------------- */
typedef struct { void **vt; int refs; bool wide; int version; } di_t;
enum { DEV_KEYBOARD, DEV_MOUSE };
typedef struct { uint32_t ofs; int kind, index; } dobj_t;      /* kind: 0 key (index = DIK), 1 axis (0 X, 1 Y, 2 Z), 2 button */
typedef struct { uint32_t ofs, data, time, seq; } ev_t;
typedef struct {
    void **vt; int refs; bool wide; int version;
    int type;
    bool acquired;
    uint32_t data_size; dobj_t obj[300]; int nobj; bool std_kbd;
    uint32_t buffer_size;
    ev_t *ev; int nev, evcap; uint32_t seq; bool overflow;
    uint8_t keys[256];                                           /* the last state seen, for buffered data */
    int mx, my; bool mpos; uint8_t mbtn[8];
    uintptr_t event;
} dev_t;

static void *com_new(int iface, size_t size) {
    build_vtables();
    di_t *o = w32_alloc(P, (uint32_t)size);
    if (!o) return NULL;
    memset(o, 0, size);
    o->vt = vtables[iface]; o->refs = 1;
    return o;
}
static HRESULT WINAPI QueryInterface(di_t *self, const uint32_t *iid, void **out) {
    if (!out) return DIERR_INVALIDPARAM;
    self->refs++; *out = self;                                   /* every DirectInput (device) version is this object */
    (void)iid;
    return DI_OK;
}
static uint32_t WINAPI AddRef(di_t *self) { return ++self->refs; }
static uint32_t WINAPI Release(di_t *self) {
    if (--self->refs > 0) return self->refs;
    if (self->vt == vtables[IF_DEV]) kfree(((dev_t *)self)->ev);
    self->vt = NULL;
    w32_free(P, self);
    return 0;
}

/* ---------------- the devices ---------------- */
/* DIK scan codes and the virtual keys behind them */
static const uint8_t dik_vk[256] = {
    [0x01] = 0x1B, [0x02] = '1', [0x03] = '2', [0x04] = '3', [0x05] = '4', [0x06] = '5', [0x07] = '6', [0x08] = '7', [0x09] = '8', [0x0A] = '9', [0x0B] = '0',
    [0x0C] = 0xBD, [0x0D] = 0xBB, [0x0E] = 0x08, [0x0F] = 0x09, [0x10] = 'Q', [0x11] = 'W', [0x12] = 'E', [0x13] = 'R', [0x14] = 'T', [0x15] = 'Y', [0x16] = 'U',
    [0x17] = 'I', [0x18] = 'O', [0x19] = 'P', [0x1A] = 0xDB, [0x1B] = 0xDD, [0x1C] = 0x0D, [0x1D] = 0x11, [0x1E] = 'A', [0x1F] = 'S', [0x20] = 'D', [0x21] = 'F',
    [0x22] = 'G', [0x23] = 'H', [0x24] = 'J', [0x25] = 'K', [0x26] = 'L', [0x27] = 0xBA, [0x28] = 0xDE, [0x29] = 0xC0, [0x2A] = 0x10, [0x2B] = 0xDC, [0x2C] = 'Z',
    [0x2D] = 'X', [0x2E] = 'C', [0x2F] = 'V', [0x30] = 'B', [0x31] = 'N', [0x32] = 'M', [0x33] = 0xBC, [0x34] = 0xBE, [0x35] = 0xBF, [0x36] = 0x10, [0x37] = 0x6A,
    [0x38] = 0x12, [0x39] = 0x20, [0x3A] = 0x14, [0x3B] = 0x70, [0x3C] = 0x71, [0x3D] = 0x72, [0x3E] = 0x73, [0x3F] = 0x74, [0x40] = 0x75, [0x41] = 0x76, [0x42] = 0x77,
    [0x43] = 0x78, [0x44] = 0x79, [0x45] = 0x90, [0x46] = 0x91, [0x47] = 0x67, [0x48] = 0x68, [0x49] = 0x69, [0x4A] = 0x6D, [0x4B] = 0x64, [0x4C] = 0x65, [0x4D] = 0x66,
    [0x4E] = 0x6B, [0x4F] = 0x61, [0x50] = 0x62, [0x51] = 0x63, [0x52] = 0x60, [0x53] = 0x6E, [0x57] = 0x7A, [0x58] = 0x7B,
    [0x9C] = 0x0D, [0x9D] = 0x11, [0xB5] = 0x6F, [0xB7] = 0x2C, [0xB8] = 0x12, [0xC5] = 0x13, [0xC7] = 0x24, [0xC8] = 0x26, [0xC9] = 0x21, [0xCB] = 0x25,
    [0xCD] = 0x27, [0xCF] = 0x23, [0xD0] = 0x28, [0xD1] = 0x22, [0xD2] = 0x2D, [0xD3] = 0x2E, [0xDB] = 0x5B, [0xDC] = 0x5C, [0xDD] = 0x5D,
};
static bool key_down(int dik) {
    int vk = dik_vk[dik & 0xFF];
    if (!vk) return false;
    /* the numeric keypad: also its digit keys when the keypad sends them */
    if (vk >= 0x60 && vk <= 0x69 && w32_vk_down(vk - 0x60 + '0') && 0) return true;
    return w32_vk_down(vk);
}
static void push(dev_t *d, uint32_t ofs, uint32_t data) {
    if (!d->buffer_size) return;
    if (d->nev >= (int)d->buffer_size) { d->overflow = true; memmove(d->ev, d->ev + 1, (d->nev - 1) * sizeof(ev_t)); d->nev--; }
    if (d->nev >= d->evcap) { d->evcap = MAX(16, d->evcap * 2); d->ev = krealloc(d->ev, d->evcap * sizeof(ev_t)); }
    d->ev[d->nev++] = (ev_t){ ofs, data, timer_ticks(), ++d->seq };
}
/* read the devices now; buffered events come from what changed since the last poll */
typedef struct { int32_t dx, dy, dz; uint8_t btn[8]; } mouse_now_t;
static void poll_keyboard(dev_t *d, uint8_t *now) {
    bool focus = w32_has_focus(P);
    for (int k = 0; k < 256; k++) {
        now[k] = focus && key_down(k) ? 0x80 : 0;
        if (now[k] != d->keys[k]) { uint32_t ofs = k; for (int i = 0; i < d->nobj; i++) if (d->obj[i].kind == 0 && d->obj[i].index == k) { ofs = d->obj[i].ofs; break; } push(d, ofs, now[k]); d->keys[k] = now[k]; }
    }
}
static uint32_t mouse_ofs(dev_t *d, int kind, int index, uint32_t def) { for (int i = 0; i < d->nobj; i++) if (d->obj[i].kind == kind && d->obj[i].index == index) return d->obj[i].ofs; return def; }
static void poll_mouse(dev_t *d, mouse_now_t *m) {
    memset(m, 0, sizeof *m);
    bool focus = w32_has_focus(P);
    int x = wm_mouse_x, y = wm_mouse_y;
    if (!d->mpos) { d->mx = x; d->my = y; d->mpos = true; }
    if (focus) { m->dx = x - d->mx; m->dy = y - d->my; m->dz = w32_wheel_take(); }
    d->mx = x; d->my = y;
    static const int vkb[3] = { 1, 2, 4 };
    for (int b = 0; b < 3; b++) m->btn[b] = focus && w32_vk_down(vkb[b]) ? 0x80 : 0;
    if (m->dx) push(d, mouse_ofs(d, 1, 0, 0), (uint32_t)m->dx);
    if (m->dy) push(d, mouse_ofs(d, 1, 1, 4), (uint32_t)m->dy);
    if (m->dz) push(d, mouse_ofs(d, 1, 2, 8), (uint32_t)m->dz);
    for (int b = 0; b < 8; b++) if (m->btn[b] != d->mbtn[b]) { push(d, mouse_ofs(d, 2, b, 12 + b), m->btn[b]); d->mbtn[b] = m->btn[b]; }
}
static HRESULT WINAPI GetDeviceState(dev_t *d, uint32_t n, void *out) {
    if (!out) return DIERR_INVALIDPARAM;
    if (!d->acquired) return DIERR_NOTACQUIRED;
    memset(out, 0, n);
    uint8_t *o = out;
    if (d->type == DEV_KEYBOARD) {
        uint8_t now[256]; poll_keyboard(d, now);
        if (d->std_kbd || !d->nobj) memcpy(o, now, MIN(n, 256u));
        else for (int i = 0; i < d->nobj; i++) if (d->obj[i].kind == 0 && d->obj[i].ofs < n) o[d->obj[i].ofs] = now[d->obj[i].index & 0xFF];
    } else {
        mouse_now_t m; poll_mouse(d, &m);
        if (!d->nobj) {                                          /* DIMOUSESTATE(2): lX lY lZ, then the buttons */
            if (n >= 12) { ((int32_t *)o)[0] = m.dx; ((int32_t *)o)[1] = m.dy; ((int32_t *)o)[2] = m.dz; }
            for (uint32_t b = 0; b < 8 && 12 + b < n; b++) o[12 + b] = m.btn[b];
        } else for (int i = 0; i < d->nobj; i++) {
            dobj_t *ob = &d->obj[i];
            if (ob->kind == 1 && ob->ofs + 4 <= n) *(int32_t *)(o + ob->ofs) = ob->index == 0 ? m.dx : ob->index == 1 ? m.dy : m.dz;
            else if (ob->kind == 2 && ob->ofs < n && ob->index < 8) o[ob->ofs] = m.btn[ob->index];
        }
    }
    return DI_OK;
}
static HRESULT WINAPI GetDeviceData(dev_t *d, uint32_t size, uint8_t *out, uint32_t *inout, uint32_t flags) {
    if (!inout) return DIERR_INVALIDPARAM;
    if (!d->acquired) return DIERR_NOTACQUIRED;
    if (d->type == DEV_KEYBOARD) { uint8_t now[256]; poll_keyboard(d, now); } else { mouse_now_t m; poll_mouse(d, &m); }
    uint32_t want = *inout, k = 0;
    if (!out && want == 0xFFFFFFFF) { HRESULT r = d->overflow ? DI_BUFFEROVERFLOW : DI_OK; d->nev = 0; d->overflow = false; *inout = 0; return r; }   /* flush */
    for (; k < want && k < (uint32_t)d->nev; k++) if (out) {
        uint8_t *e = out + k * size; memset(e, 0, size);
        uint32_t f[4] = { d->ev[k].ofs, d->ev[k].data, d->ev[k].time, d->ev[k].seq };
        memcpy(e, f, MIN(size, 16u));
    }
    *inout = k;
    HRESULT r = d->overflow ? DI_BUFFEROVERFLOW : DI_OK;
    if (!(flags & 1)) { memmove(d->ev, d->ev + k, (d->nev - k) * sizeof(ev_t)); d->nev -= k; d->overflow = false; }   /* not DIGDD_PEEK */
    return r;
}
/* DIDATAFORMAT: dwSize, dwObjSize, dwFlags, dwDataSize, dwNumObjs, rgodf (pguid, dwOfs, dwType, dwFlags) */
static HRESULT WINAPI SetDataFormat(dev_t *d, const uint8_t *df) {
    if (!df) return DIERR_INVALIDPARAM;
    if (d->acquired) return (HRESULT)0x80070005;              /* DIERR_ACQUIRED */
    uint32_t objsize = *(const uint32_t *)(df + 4), datasize = *(const uint32_t *)(df + 12), n = *(const uint32_t *)(df + 16);
    const uint8_t *objs = *(const uint8_t *const *)(df + (sizeof(void *) == 8 ? 24 : 20));   /* rgodf, pointer-aligned */
    d->data_size = datasize; d->nobj = 0; d->std_kbd = d->type == DEV_KEYBOARD && datasize == 256;
    int axis_i = 0, button_i = 0;
    for (uint32_t i = 0; i < n && objs && d->nobj < 300; i++) {
        const uint8_t *o = objs + i * objsize;
        const uint32_t *guid = *(const uint32_t *const *)o;
        uint32_t ofs = *(const uint32_t *)(o + sizeof(void *)), type = *(const uint32_t *)(o + sizeof(void *) + 4);
        uint32_t inst = (type >> 8) & 0xFFFF; bool any = inst == 0xFFFF;
        dobj_t *ob = &d->obj[d->nobj];
        ob->ofs = ofs;
        if (d->type == DEV_KEYBOARD) { if (type & 0xC) { ob->kind = 0; ob->index = any ? (int)i : (int)inst; d->nobj++; } continue; }
        if (type & 3) {                                           /* an axis: X, Y or Z by its GUID, else in order */
            ob->kind = 1;
            if (guid && (guid[0] & 0xFFFFFF00) == 0xA36D0200) ob->index = (guid[0] & 0xFF) - 0xE0; else ob->index = any ? axis_i : (int)inst;
            axis_i++; d->nobj++;
        } else if (type & 0xC) { ob->kind = 2; ob->index = any ? button_i : (int)inst; button_i++; d->nobj++; }
    }
    if (d->type == DEV_MOUSE && datasize >= 16 && d->nobj && d->obj[0].ofs == 0 && d->nobj >= 3) {}   /* c_dfDIMouse(2): handled by the object list */
    return DI_OK;
}
static HRESULT WINAPI Acquire(dev_t *d) { d->acquired = true; d->mpos = false; if (d->type == DEV_KEYBOARD) for (int k = 0; k < 256; k++) d->keys[k] = key_down(k) ? 0x80 : 0; return DI_OK; }
static HRESULT WINAPI Unacquire(dev_t *d) { d->acquired = false; return DI_OK; }
static HRESULT WINAPI SetCooperativeLevel(dev_t *d, uintptr_t hwnd, uint32_t flags) { (void)d; (void)hwnd; (void)flags; return DI_OK; }
static HRESULT WINAPI SetEventNotification(dev_t *d, uintptr_t ev) { d->event = ev; return DI_OK; }
static HRESULT WINAPI Poll(dev_t *d) { return d->acquired ? 1 : DIERR_NOTACQUIRED; }    /* DI_NOEFFECT: these devices are not polled */
/* DIDEVCAPS */
static HRESULT WINAPI GetCapabilities(dev_t *d, uint32_t *c) {
    if (!c || c[0] < 24) return DIERR_INVALIDPARAM;
    uint32_t sz = c[0]; memset(c + 1, 0, sz - 4);
    c[1] = 1 | 4;                                                /* DIDC_ATTACHED, DIDC_EMULATED */
    if (d->type == DEV_KEYBOARD) { c[2] = d->version >= 0x800 ? 0x413 : 0x403; c[4] = 128; }
    else { c[2] = d->version >= 0x800 ? 0x212 : 0x202; c[3] = 3; c[4] = 3; }
    return DI_OK;
}
/* DIPROPHEADER: dwSize, dwHeaderSize, dwObj, dwHow; then the value */
static HRESULT WINAPI SetProperty(dev_t *d, uintptr_t prop, const uint32_t *ph) {
    if (!ph) return DIERR_INVALIDPARAM;
    if (prop == 1) { d->buffer_size = MIN(ph[4], 4096u); return DI_OK; }   /* DIPROP_BUFFERSIZE */
    return DI_OK;                                                /* axis mode, granularity, range: accepted */
}
static HRESULT WINAPI GetProperty(dev_t *d, uintptr_t prop, uint32_t *ph) {
    if (!ph) return DIERR_INVALIDPARAM;
    switch (prop) {
    case 1: ph[4] = d->buffer_size; return DI_OK;               /* DIPROP_BUFFERSIZE */
    case 2: ph[4] = 0; return DI_OK;                             /* DIPROP_AXISMODE: relative */
    case 3: ph[4] = ph[2] == 8 ? 120 : 1; return DI_OK;          /* DIPROP_GRANULARITY: the wheel moves 120 a notch */
    case 4: ((int32_t *)ph)[4] = -0x7FFFFFFF; ((int32_t *)ph)[5] = 0x7FFFFFFF; return DI_OK;   /* DIPROP_RANGE */
    }
    return DIERR_UNSUPPORTED;
}
/* DIDEVICEINSTANCE: dwSize, guidInstance, guidProduct, dwDevType, tszInstanceName[260], tszProductName[260], guidFFDriver, wUsagePage, wUsage */
static const uint32_t guid_keyboard[4] = { 0x6F1D2B61, 0x11CFD5A0, 0x4544C7BF, 0x00005453 };
static const uint32_t guid_mouse[4] = { 0x6F1D2B60, 0x11CFD5A0, 0x4544C7BF, 0x00005453 };
static void fill_instance(uint8_t *di, bool wide, int type, int version) {
    uint32_t sz = *(uint32_t *)di; memset(di + 4, 0, sz - 4);
    memcpy(di + 4, type == DEV_KEYBOARD ? guid_keyboard : guid_mouse, 16);
    memcpy(di + 20, type == DEV_KEYBOARD ? guid_keyboard : guid_mouse, 16);
    *(uint32_t *)(di + 36) = type == DEV_KEYBOARD ? (version >= 0x800 ? 0x413 : 0x403) : (version >= 0x800 ? 0x212 : 0x202);
    const char *name = type == DEV_KEYBOARD ? "Keyboard" : "Mouse";
    int cs = wide ? 2 : 1;
    for (int i = 0; name[i]; i++) { if (wide) { ((uint16_t *)(di + 40))[i] = name[i]; ((uint16_t *)(di + 40 + 260 * cs))[i] = name[i]; } else { di[40 + i] = name[i]; di[40 + 260 + i] = name[i]; } }
    uint32_t usage_at = 40 + 520 * cs + 16;
    if (sz >= usage_at + 4) { *(uint16_t *)(di + usage_at) = 1; *(uint16_t *)(di + usage_at + 2) = type == DEV_KEYBOARD ? 6 : 2; }
}
static HRESULT WINAPI GetDeviceInfo(dev_t *d, uint8_t *di) { if (!di) return DIERR_INVALIDPARAM; fill_instance(di, d->wide, d->type, d->version); return DI_OK; }
/* EnumObjects: the mouse's axes and buttons (DIDEVICEOBJECTINSTANCE: dwSize, guidType, dwOfs, dwType, dwFlags, tszName[260], ...) */
static HRESULT WINAPI EnumObjects(dev_t *d, void *cb, void *ref, uint32_t flags) {
    if (!cb) return DIERR_INVALIDPARAM;
    if (d->type != DEV_MOUSE) return DI_OK;
    static const char *names[] = { "X-axis", "Y-axis", "Wheel", "Left button", "Right button", "Middle button" };
    for (int i = 0; i < 6; i++) {
        bool axis = i < 3;
        if (flags && !(flags & (axis ? 3 : 0xC))) continue;
        uint8_t oi[600]; memset(oi, 0, sizeof oi);
        int cs = d->wide ? 2 : 1;
        *(uint32_t *)oi = 4 + 16 + 12 + 260 * cs;
        uint32_t g[4] = { axis ? 0xA36D02E0u + i : 0xA36D02F0u, 0x11CFC9F3, 0x0000E2BF, 0x00005354 };
        memcpy(oi + 4, g, 16);
        *(uint32_t *)(oi + 20) = axis ? i * 4 : 12 + (i - 3);
        *(uint32_t *)(oi + 24) = (axis ? 1 : 4) | (axis ? i : i - 3) << 8;
        for (int k = 0; names[i][k]; k++) { if (d->wide) ((uint16_t *)(oi + 32))[k] = names[i][k]; else oi[32 + k] = names[i][k]; }
        if (!((int (WINAPI *)(const void *, void *))cb)(oi, ref)) break;
    }
    return DI_OK;
}
static HRESULT WINAPI GetObjectInfo(dev_t *d, void *oi, uint32_t obj, uint32_t how) { (void)d; (void)oi; (void)obj; (void)how; return DIERR_OBJECTNOTFOUND; }
static HRESULT WINAPI RunControlPanelDev(dev_t *d, uintptr_t h, uint32_t f) { (void)d; (void)h; (void)f; return DI_OK; }
static HRESULT WINAPI InitializeDev(dev_t *d, uintptr_t inst, uint32_t ver, const void *g) { (void)d; (void)inst; (void)ver; (void)g; return DI_OK; }
static HRESULT WINAPI EnumEffects(dev_t *d, void *cb, void *ref, uint32_t t) { (void)d; (void)cb; (void)ref; (void)t; return DI_OK; }
static HRESULT WINAPI GetForceFeedbackState(dev_t *d, uint32_t *s) { (void)d; if (s) *s = 0; return DIERR_UNSUPPORTED; }
static HRESULT WINAPI SendForceFeedbackCommand(dev_t *d, uint32_t c) { (void)d; (void)c; return DIERR_UNSUPPORTED; }
static HRESULT WINAPI EnumCreatedEffectObjects(dev_t *d, void *cb, void *ref, uint32_t f) { (void)d; (void)cb; (void)ref; (void)f; return DI_OK; }
static HRESULT WINAPI Escape(dev_t *d, void *e) { (void)d; (void)e; return DIERR_UNSUPPORTED; }
static HRESULT WINAPI SendDeviceData(dev_t *d, uint32_t sz, const void *dat, uint32_t *n, uint32_t f) { (void)d; (void)sz; (void)dat; if (n) *n = 0; (void)f; return DI_OK; }
static HRESULT WINAPI EnumEffectsInFile(dev_t *d, const void *f, void *cb, void *ref, uint32_t fl) { (void)d; (void)f; (void)cb; (void)ref; (void)fl; return DI_OK; }
static HRESULT WINAPI BuildActionMap(dev_t *d, void *m, const void *u, uint32_t f) { (void)d; (void)m; (void)u; (void)f; return DIERR_UNSUPPORTED; }

/* ---------------- IDirectInput8 / IDirectInput7 ---------------- */
static HRESULT WINAPI CreateDevice(di_t *di, const uint32_t *guid, dev_t **out, void *outer) {
    if (!guid || !out) return DIERR_INVALIDPARAM;
    *out = NULL;
    int type = !memcmp(guid, guid_keyboard, 16) ? DEV_KEYBOARD : !memcmp(guid, guid_mouse, 16) ? DEV_MOUSE : -1;
    if (type < 0) return DIERR_DEVICENOTREG;                     /* no joysticks */
    dev_t *d = com_new(IF_DEV, sizeof(dev_t));
    if (!d) return DIERR_INVALIDPARAM;
    d->type = type; d->wide = di->wide; d->version = di->version;
    *out = d;
    return DI_OK;
}
static HRESULT WINAPI CreateDeviceEx(di_t *di, const uint32_t *guid, const void *iid, dev_t **out, void *outer) { return CreateDevice(di, guid, out, outer); }
/* EnumDevices: the keyboard and the mouse for "all", "keyboard", "pointer" (DX8 classes and DX7 types) */
static HRESULT WINAPI EnumDevices(di_t *di, uint32_t type, void *cb, void *ref, uint32_t flags) {
    if (!cb) return DIERR_INVALIDPARAM;
    uint32_t t = type & 0xFF;
    for (int dev = DEV_KEYBOARD; dev <= DEV_MOUSE; dev++) {
        bool want = t == 0 || t == 1 || (dev == DEV_KEYBOARD && t == 3) || (dev == DEV_MOUSE && t == 2) || (di->version >= 0x800 && ((dev == DEV_KEYBOARD && t == 0x13) || (dev == DEV_MOUSE && t == 0x12)));
        if (!want) continue;
        uint8_t inst[1200]; memset(inst, 0, sizeof inst);
        *(uint32_t *)inst = di->wide ? (di->version >= 0x500 ? 1092 : 1060) : (di->version >= 0x500 ? 580 : 564);
        fill_instance(inst, di->wide, dev, di->version);
        if (!((int (WINAPI *)(const void *, void *))cb)(inst, ref)) break;
    }
    return DI_OK;
}
static HRESULT WINAPI GetDeviceStatus(di_t *di, const uint32_t *guid) { return guid && (!memcmp(guid, guid_keyboard, 16) || !memcmp(guid, guid_mouse, 16)) ? DI_OK : 1; }   /* DI_NOTATTACHED */
static HRESULT WINAPI RunControlPanel(di_t *di, uintptr_t h, uint32_t f) { (void)di; (void)h; (void)f; return DI_OK; }
static HRESULT WINAPI Initialize(di_t *di, uintptr_t inst, uint32_t ver) { di->version = ver; return DI_OK; }
static HRESULT WINAPI FindDevice(di_t *di, const void *cls, const void *name, void *guid) { (void)di; (void)cls; (void)name; (void)guid; return DIERR_DEVICENOTREG; }
static HRESULT WINAPI EnumDevicesBySemantics(di_t *di, const void *u, void *af, void *cb, void *ref, uint32_t f) { (void)di; (void)u; (void)af; (void)cb; (void)ref; (void)f; return DI_OK; }
static HRESULT WINAPI ConfigureDevices(di_t *di, void *cb, void *p, uint32_t f, void *ref) { (void)di; (void)cb; (void)p; (void)f; (void)ref; return DIERR_UNSUPPORTED; }

static const impl_t impls[] = {
#define BOTH(n, f) { IF_DI8, n, (void *)f }, { IF_DI7, n, (void *)f }
    BOTH("QueryInterface", QueryInterface), BOTH("AddRef", AddRef), BOTH("Release", Release), BOTH("CreateDevice", CreateDevice), BOTH("EnumDevices", EnumDevices),
    BOTH("GetDeviceStatus", GetDeviceStatus), BOTH("RunControlPanel", RunControlPanel), BOTH("Initialize", Initialize), BOTH("FindDevice", FindDevice),
    { IF_DI7, "CreateDeviceEx", (void *)CreateDeviceEx }, { IF_DI8, "EnumDevicesBySemantics", (void *)EnumDevicesBySemantics }, { IF_DI8, "ConfigureDevices", (void *)ConfigureDevices },
#define D(n, f) { IF_DEV, n, (void *)f }
    D("QueryInterface", QueryInterface), D("AddRef", AddRef), D("Release", Release), D("GetCapabilities", GetCapabilities), D("EnumObjects", EnumObjects),
    D("GetProperty", GetProperty), D("SetProperty", SetProperty), D("Acquire", Acquire), D("Unacquire", Unacquire), D("GetDeviceState", GetDeviceState),
    D("GetDeviceData", GetDeviceData), D("SetDataFormat", SetDataFormat), D("SetEventNotification", SetEventNotification), D("SetCooperativeLevel", SetCooperativeLevel),
    D("GetObjectInfo", GetObjectInfo), D("GetDeviceInfo", GetDeviceInfo), D("RunControlPanel", RunControlPanelDev), D("Initialize", InitializeDev),
    D("EnumEffects", EnumEffects), D("GetForceFeedbackState", GetForceFeedbackState), D("SendForceFeedbackCommand", SendForceFeedbackCommand),
    D("EnumCreatedEffectObjects", EnumCreatedEffectObjects), D("Escape", Escape), D("Poll", Poll), D("SendDeviceData", SendDeviceData),
    D("EnumEffectsInFile", EnumEffectsInFile), D("BuildActionMap", BuildActionMap),
};
static void build_vtables(void) {
    if (vt_ready) return;
    for (unsigned i = 0; i < ARRAY_LEN(methods); i++) {
        const method_t *m = &methods[i];
        if (m->iface < 0) continue;
        void *fn = stubs[MIN(m->nargs, 6)];
        for (unsigned k = 0; k < ARRAY_LEN(impls); k++) if (impls[k].iface == m->iface && !strcmp(impls[k].name, m->name)) { fn = impls[k].fn; break; }
        vtables[m->iface][m->index] = fn;
    }
    vt_ready = true;
}

/* ---------------- exports ---------------- */
static HRESULT WINAPI DirectInput8Create(uintptr_t inst, uint32_t ver, const uint32_t *iid, void **out, void *outer) {
    if (!out) return DIERR_INVALIDPARAM;
    di_t *d = com_new(IF_DI8, sizeof(di_t));
    if (!d) return DIERR_INVALIDPARAM;
    d->wide = iid && (iid[0] & 1); d->version = ver ? ver : 0x800;
    *out = d;
    return DI_OK;
}
static HRESULT WINAPI DirectInputCreateEx(uintptr_t inst, uint32_t ver, const uint32_t *iid, void **out, void *outer) {
    if (!out) return DIERR_INVALIDPARAM;
    di_t *d = com_new(IF_DI7, sizeof(di_t));
    if (!d) return DIERR_INVALIDPARAM;
    d->wide = iid && (iid[0] & 1); d->version = ver ? ver : 0x700;
    *out = d;
    return DI_OK;
}
static HRESULT WINAPI DirectInputCreateA(uintptr_t inst, uint32_t ver, void **out, void *outer) { static const uint32_t a[1] = { 0x89521360 }; return DirectInputCreateEx(inst, ver, a, out, outer); }
static HRESULT WINAPI DirectInputCreateW(uintptr_t inst, uint32_t ver, void **out, void *outer) { static const uint32_t w[1] = { 0x89521361 }; return DirectInputCreateEx(inst, ver, w, out, outer); }
static HRESULT WINAPI DllCanUnloadNow(void) { return 1; }

#define X(g, n) { g, #n, (void *)n }
const w32_api_t dinput_api[] = {
    X("dinput8", DirectInput8Create), X("dinput8", DllCanUnloadNow), X("dinput", DirectInputCreateA), X("dinput", DirectInputCreateW), X("dinput", DirectInputCreateEx),
    X("dinput", DllCanUnloadNow), { "dinput", "DirectInputCreate", (void *)DirectInputCreateA },
    { NULL, NULL, NULL }
};
