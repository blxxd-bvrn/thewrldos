/* A stand-in libEGL.so.1 for Linux programs on THE WRLD OS, which has no GPU
 * driver for them. WebKitGTK insists on an EGL display even when it renders on
 * the CPU (WEBKIT_SKIA_ENABLE_CPU_RENDERING): this one lets displays be opened
 * and initialised, and refuses every context and surface, so programs fall
 * back to their software paths instead of aborting. It replaces glvnd's
 * dispatcher and Mesa (about 180 MB with LLVM) in the OS's Linux root. */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <stdint.h>
#include <string.h>

typedef int32_t EGLint;
typedef unsigned int EGLBoolean, EGLenum;
typedef void *EGLDisplay, *EGLConfig, *EGLContext, *EGLSurface, *EGLSync, *EGLDeviceEXT, *EGLImage;
typedef intptr_t EGLAttrib;
#define EXPORT __attribute__((visibility("default")))

enum {
    EGL_SUCCESS = 0x3000, EGL_NOT_INITIALIZED = 0x3001, EGL_BAD_ALLOC = 0x3003, EGL_BAD_ATTRIBUTE = 0x3004,
    EGL_BAD_CONFIG = 0x3005, EGL_BAD_DISPLAY = 0x3008, EGL_BAD_MATCH = 0x3009, EGL_BAD_PARAMETER = 0x300C,
    EGL_VENDOR = 0x3053, EGL_VERSION = 0x3054, EGL_EXTENSIONS = 0x3055, EGL_CLIENT_APIS = 0x308D,
    EGL_OPENGL_ES_API = 0x30A0,
};

static struct { int initialised; } the_display;
static EGLint last_error = EGL_SUCCESS;
static EGLenum api = EGL_OPENGL_ES_API;
static EGLint fail(EGLint e) { last_error = e; return 0; }
static int ours(EGLDisplay d) { return d == (EGLDisplay)&the_display; }

EXPORT EGLint eglGetError(void) { EGLint e = last_error; last_error = EGL_SUCCESS; return e; }
EXPORT EGLDisplay eglGetDisplay(void *native) { (void)native; return &the_display; }
EXPORT EGLDisplay eglGetPlatformDisplay(EGLenum platform, void *native, const EGLAttrib *attrs) { (void)platform; (void)native; (void)attrs; return &the_display; }
EXPORT EGLDisplay eglGetPlatformDisplayEXT(EGLenum platform, void *native, const EGLint *attrs) { (void)platform; (void)native; (void)attrs; return &the_display; }
EXPORT EGLBoolean eglInitialize(EGLDisplay d, EGLint *major, EGLint *minor) {
    if (!ours(d)) return fail(EGL_BAD_DISPLAY);
    the_display.initialised = 1;
    if (major) *major = 1;
    if (minor) *minor = 5;
    return 1;
}
EXPORT EGLBoolean eglTerminate(EGLDisplay d) { if (!ours(d)) return fail(EGL_BAD_DISPLAY); the_display.initialised = 0; return 1; }
EXPORT const char *eglQueryString(EGLDisplay d, EGLint name) {
    if (!d) {                                             /* client extensions: what platforms can be asked for */
        if (name == EGL_EXTENSIONS) return "EGL_EXT_client_extensions EGL_EXT_platform_base EGL_KHR_platform_wayland "
                                           "EGL_EXT_platform_wayland EGL_MESA_platform_surfaceless EGL_KHR_platform_gbm";
        if (name == EGL_VERSION) return "1.5 WRLD";
        fail(EGL_BAD_DISPLAY); return NULL;
    }
    if (!ours(d)) { fail(EGL_BAD_DISPLAY); return NULL; }
    if (!the_display.initialised) { fail(EGL_NOT_INITIALIZED); return NULL; }
    switch (name) {
    case EGL_VENDOR: return "THE WRLD OS (no GPU)";
    case EGL_VERSION: return "1.5 WRLD";
    case EGL_EXTENSIONS: return "";
    case EGL_CLIENT_APIS: return "OpenGL_ES";
    default: fail(EGL_BAD_PARAMETER); return NULL;
    }
}
EXPORT EGLBoolean eglQueryDisplayAttribEXT(EGLDisplay d, EGLint attr, EGLAttrib *value) { (void)d; (void)attr; (void)value; return fail(EGL_BAD_ATTRIBUTE); }
EXPORT EGLBoolean eglQueryDisplayAttribKHR(EGLDisplay d, EGLint attr, EGLAttrib *value) { (void)d; (void)attr; (void)value; return fail(EGL_BAD_ATTRIBUTE); }
EXPORT const char *eglQueryDeviceStringEXT(EGLDeviceEXT dev, EGLint name) { (void)dev; (void)name; fail(EGL_BAD_PARAMETER); return NULL; }
EXPORT EGLBoolean eglQueryDevicesEXT(EGLint max, EGLDeviceEXT *devs, EGLint *n) { (void)max; (void)devs; if (n) *n = 0; return 1; }
EXPORT EGLBoolean eglBindAPI(EGLenum a) { api = a; return 1; }
EXPORT EGLenum eglQueryAPI(void) { return api; }
EXPORT EGLBoolean eglChooseConfig(EGLDisplay d, const EGLint *attrs, EGLConfig *configs, EGLint size, EGLint *n) {
    (void)attrs; (void)configs; (void)size;
    if (n) *n = 0;
    return ours(d) ? 1 : fail(EGL_BAD_DISPLAY);
}
EXPORT EGLBoolean eglGetConfigs(EGLDisplay d, EGLConfig *configs, EGLint size, EGLint *n) { return eglChooseConfig(d, NULL, configs, size, n); }
EXPORT EGLBoolean eglGetConfigAttrib(EGLDisplay d, EGLConfig c, EGLint attr, EGLint *v) { (void)d; (void)c; (void)attr; (void)v; return fail(EGL_BAD_CONFIG); }
EXPORT EGLContext eglCreateContext(EGLDisplay d, EGLConfig c, EGLContext share, const EGLint *attrs) { (void)d; (void)c; (void)share; (void)attrs; fail(EGL_BAD_MATCH); return NULL; }
EXPORT EGLBoolean eglDestroyContext(EGLDisplay d, EGLContext c) { (void)d; (void)c; return 1; }
EXPORT EGLBoolean eglQueryContext(EGLDisplay d, EGLContext c, EGLint a, EGLint *v) { (void)d; (void)c; (void)a; (void)v; return fail(EGL_BAD_PARAMETER); }
EXPORT EGLBoolean eglCopyBuffers(EGLDisplay d, EGLSurface s, void *p) { (void)d; (void)s; (void)p; return fail(EGL_BAD_PARAMETER); }
EXPORT EGLBoolean eglSurfaceAttrib(EGLDisplay d, EGLSurface s, EGLint a, EGLint v) { (void)d; (void)s; (void)a; (void)v; return fail(EGL_BAD_PARAMETER); }
EXPORT EGLBoolean eglBindTexImage(EGLDisplay d, EGLSurface s, EGLint b) { (void)d; (void)s; (void)b; return fail(EGL_BAD_PARAMETER); }
EXPORT EGLBoolean eglReleaseTexImage(EGLDisplay d, EGLSurface s, EGLint b) { (void)d; (void)s; (void)b; return fail(EGL_BAD_PARAMETER); }
EXPORT EGLSurface eglCreatePixmapSurface(EGLDisplay d, EGLConfig c, void *p, const EGLint *a) { (void)d; (void)c; (void)p; (void)a; fail(EGL_BAD_CONFIG); return NULL; }
EXPORT EGLSurface eglCreatePlatformPixmapSurface(EGLDisplay d, EGLConfig c, void *p, const EGLAttrib *a) { (void)d; (void)c; (void)p; (void)a; fail(EGL_BAD_CONFIG); return NULL; }
EXPORT EGLSurface eglCreatePbufferFromClientBuffer(EGLDisplay d, EGLenum t, void *b, EGLConfig c, const EGLint *a) { (void)d; (void)t; (void)b; (void)c; (void)a; fail(EGL_BAD_CONFIG); return NULL; }
EXPORT EGLint eglClientWaitSync(EGLDisplay d, EGLSync s, EGLint f, uint64_t t) { (void)d; (void)s; (void)f; (void)t; return fail(EGL_BAD_PARAMETER); }
EXPORT EGLBoolean eglWaitSync(EGLDisplay d, EGLSync s, EGLint f) { (void)d; (void)s; (void)f; return fail(EGL_BAD_PARAMETER); }
EXPORT EGLBoolean eglGetSyncAttrib(EGLDisplay d, EGLSync s, EGLint a, EGLAttrib *v) { (void)d; (void)s; (void)a; (void)v; return fail(EGL_BAD_PARAMETER); }
EXPORT EGLSurface eglCreateWindowSurface(EGLDisplay d, EGLConfig c, void *w, const EGLint *a) { (void)d; (void)c; (void)w; (void)a; fail(EGL_BAD_CONFIG); return NULL; }
EXPORT EGLSurface eglCreatePlatformWindowSurface(EGLDisplay d, EGLConfig c, void *w, const EGLAttrib *a) { (void)d; (void)c; (void)w; (void)a; fail(EGL_BAD_CONFIG); return NULL; }
EXPORT EGLSurface eglCreatePbufferSurface(EGLDisplay d, EGLConfig c, const EGLint *a) { (void)d; (void)c; (void)a; fail(EGL_BAD_CONFIG); return NULL; }
EXPORT EGLBoolean eglDestroySurface(EGLDisplay d, EGLSurface s) { (void)d; (void)s; return 1; }
EXPORT EGLBoolean eglQuerySurface(EGLDisplay d, EGLSurface s, EGLint a, EGLint *v) { (void)d; (void)s; (void)a; (void)v; return fail(EGL_BAD_PARAMETER); }
EXPORT EGLBoolean eglMakeCurrent(EGLDisplay d, EGLSurface dr, EGLSurface rd, EGLContext c) { (void)d; return (!dr && !rd && !c) ? 1 : fail(EGL_BAD_MATCH); }
EXPORT EGLContext eglGetCurrentContext(void) { return NULL; }
EXPORT EGLDisplay eglGetCurrentDisplay(void) { return NULL; }
EXPORT EGLSurface eglGetCurrentSurface(EGLint which) { (void)which; return NULL; }
EXPORT EGLBoolean eglSwapBuffers(EGLDisplay d, EGLSurface s) { (void)d; (void)s; return fail(EGL_BAD_PARAMETER); }
EXPORT EGLBoolean eglSwapInterval(EGLDisplay d, EGLint i) { (void)d; (void)i; return 1; }
EXPORT EGLBoolean eglWaitClient(void) { return 1; }
EXPORT EGLBoolean eglWaitGL(void) { return 1; }
EXPORT EGLBoolean eglWaitNative(EGLint e) { (void)e; return 1; }
EXPORT EGLBoolean eglReleaseThread(void) { return 1; }
EXPORT EGLSync eglCreateSync(EGLDisplay d, EGLenum t, const EGLAttrib *a) { (void)d; (void)t; (void)a; fail(EGL_BAD_PARAMETER); return NULL; }
EXPORT EGLBoolean eglDestroySync(EGLDisplay d, EGLSync s) { (void)d; (void)s; return 1; }
EXPORT EGLImage eglCreateImage(EGLDisplay d, EGLContext c, EGLenum t, void *b, const EGLAttrib *a) { (void)d; (void)c; (void)t; (void)b; (void)a; fail(EGL_BAD_PARAMETER); return NULL; }
EXPORT EGLBoolean eglDestroyImage(EGLDisplay d, EGLImage i) { (void)d; (void)i; return 1; }
/* everything else by name from this library; unknown names give NULL, so callers see "unsupported" */
EXPORT void (*eglGetProcAddress(const char *name))(void) {
    void *f = name && !strncmp(name, "egl", 3) ? dlsym(RTLD_DEFAULT, name) : NULL;
    Dl_info info;
    if (f && dladdr(f, &info) && dladdr((void *)eglGetProcAddress, &info)) {
        Dl_info mine; dladdr((void *)eglGetProcAddress, &mine); dladdr(f, &info);
        if (info.dli_fbase != mine.dli_fbase) f = NULL;   /* only our own functions */
    }
    return (void (*)(void))f;
}
