/* vulkan-1.dll: the Vulkan loader, honestly reporting that there is no Vulkan
 * device. Vulkan draws only with SPIR-V shader programs, which the WRLD 3D
 * software engine cannot run, so programs that try Vulkan first get
 * VK_ERROR_INCOMPATIBLE_DRIVER and can fall back to Direct3D 9 or OpenGL, which
 * THE WRLD OS does provide (d3d9.c, gl.c). */
#include "w32.h"

#define VK_SUCCESS 0
#define VK_ERROR_INITIALIZATION_FAILED (-3)
#define VK_ERROR_EXTENSION_NOT_PRESENT (-7)
#define VK_ERROR_INCOMPATIBLE_DRIVER (-9)

static int32_t WINAPI vkCreateInstance(const void *info, const void *alloc, void **inst) {
    (void)info; (void)alloc;
    if (inst) *inst = NULL;
    klog("w32: Vulkan requested: no Vulkan device (use Direct3D 9 or OpenGL)");
    return VK_ERROR_INCOMPATIBLE_DRIVER;
}
static void WINAPI vkDestroyInstance(void *inst, const void *alloc) { (void)inst; (void)alloc; }
static int32_t WINAPI vkEnumeratePhysicalDevices(void *inst, uint32_t *count, void **devs) { (void)inst; (void)devs; if (count) *count = 0; return VK_SUCCESS; }
static int32_t WINAPI vkEnumerateInstanceExtensionProperties(const char *layer, uint32_t *count, void *props) {
    (void)props; if (count) *count = 0; return layer ? VK_ERROR_EXTENSION_NOT_PRESENT : VK_SUCCESS;
}
static int32_t WINAPI vkEnumerateInstanceLayerProperties(uint32_t *count, void *props) { (void)props; if (count) *count = 0; return VK_SUCCESS; }
static int32_t WINAPI vkEnumerateInstanceVersion(uint32_t *v) { if (v) *v = (1u << 22) | (0 << 12); return VK_SUCCESS; }   /* 1.0 */

static void *WINAPI vkGetInstanceProcAddr(void *inst, const char *name);
#define E(n) { #n, (void *)n }
static const w32_export_t lookup_table[] = {
    E(vkCreateInstance), E(vkDestroyInstance), E(vkEnumeratePhysicalDevices), E(vkEnumerateInstanceExtensionProperties),
    E(vkEnumerateInstanceLayerProperties), E(vkEnumerateInstanceVersion), E(vkGetInstanceProcAddr),
};
static void *WINAPI vkGetInstanceProcAddr(void *inst, const char *name) {
    (void)inst;
    if (!name) return NULL;
    for (unsigned i = 0; i < ARRAY_LEN(lookup_table); i++) if (!strcmp(lookup_table[i].name, name)) return lookup_table[i].fn;
    return NULL;
}
static void *WINAPI vkGetDeviceProcAddr(void *dev, const char *name) { (void)dev; (void)name; return NULL; }

const w32_export_t vulkan_exports[] = {
    E(vkCreateInstance), E(vkDestroyInstance), E(vkEnumeratePhysicalDevices), E(vkEnumerateInstanceExtensionProperties),
    E(vkEnumerateInstanceLayerProperties), E(vkEnumerateInstanceVersion), E(vkGetInstanceProcAddr), E(vkGetDeviceProcAddr),
};
const int vulkan_count = ARRAY_LEN(vulkan_exports);
