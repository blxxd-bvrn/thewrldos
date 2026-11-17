/* Native WRLD shader-extension probe. Uses the D3D10 upload API, not D3D11/12;
 * run with 3D acceleration enabled. Real DXC rejection fixtures are embedded. */
#define COBJMACROS
#define __USE_MINGW_ANSI_STDIO 0
#include <windows.h>
#include <d3d10.h>
#include <stdio.h>
#include <string.h>
#include "d3d10cube_shaders.h"
#include "d3d10cube_modern_shaders.h"
#include "shaderprobe_shaders.h"

#if defined(__MINGW32__) && defined(__i386__)
/* Avoid the CRT's optional SSE2-using math diagnostic printer on x87 WRLD. */
struct _exception;
int _matherr(struct _exception *e) { (void)e; return 0; }
#endif

static int checks, failures;
static void check(const char *name, int good) {
    checks++;
    if (!good) { char text[200]; failures++; sprintf(text, "SHADERPROBE FAIL %s", name); puts(text); OutputDebugStringA(text); }
}
static void upload(ID3D10Device *dev, const char *name, const void *code, SIZE_T size, int vertex, int supported) {
    HRESULT hr;
    if (vertex) {
        ID3D10VertexShader *s = NULL; hr = ID3D10Device_CreateVertexShader(dev, code, size, &s);
        check(name, supported ? SUCCEEDED(hr) && s != NULL : FAILED(hr) && s == NULL);
        if (s) ID3D10VertexShader_Release(s);
    } else {
        ID3D10PixelShader *s = NULL; hr = ID3D10Device_CreatePixelShader(dev, code, size, &s);
        check(name, supported ? SUCCEEDED(hr) && s != NULL : FAILED(hr) && s == NULL);
        if (s) ID3D10PixelShader_Release(s);
    }
}
int main(void) {
    ID3D10Device *dev = NULL;
    HRESULT hr = D3D10CreateDevice(NULL, D3D10_DRIVER_TYPE_HARDWARE, NULL, 0, D3D10_SDK_VERSION, &dev);
    if (FAILED(hr) || !dev) { OutputDebugStringA("SHADERPROBE FAIL device creation (enable 3D acceleration)"); return 1; }
    check("D3D10 VS profile remains SM4", !strcmp(D3D10GetVertexShaderProfile(dev), "vs_4_0"));
    check("D3D10 PS profile remains SM4", !strcmp(D3D10GetPixelShaderProfile(dev), "ps_4_0"));
    upload(dev, "SM4 VS", g_vs, sizeof g_vs, 1, 1); upload(dev, "SM4 PS", g_ps, sizeof g_ps, 0, 1);
    upload(dev, "SM5 VS", g_vs5, sizeof g_vs5, 1, 1); upload(dev, "SM5 PS", g_ps5, sizeof g_ps5, 0, 1);
    upload(dev, "SM6 VS", g_vs6, sizeof g_vs6, 1, 1); upload(dev, "SM6 PS", g_ps6, sizeof g_ps6, 0, 1);
    upload(dev, "wrong stage", g_vs6, sizeof g_vs6, 0, 0);
    upload(dev, "truncated container", g_ps6, sizeof g_ps6 - 1, 0, 0);
    upload(dev, "wave operations rejected", g_reject_wave, sizeof g_reject_wave, 0, 0);
    upload(dev, "compute rejected", g_reject_compute, sizeof g_reject_compute, 0, 0);
    upload(dev, "nonzero LOD rejected", g_reject_lod, sizeof g_reject_lod, 0, 0);
    upload(dev, "resource spaces rejected", g_reject_space, sizeof g_reject_space, 0, 0);
    upload(dev, "UAVs rejected", g_reject_uav, sizeof g_reject_uav, 0, 0);
    upload(dev, "SM6.1 rejected", g_reject_profile, sizeof g_reject_profile, 0, 0);
    ID3D10Device_Release(dev);
    char result[160]; sprintf(result, "SHADERPROBE %s %d checks", failures ? "FAILED" : "passed", checks);
    puts(result); OutputDebugStringA(result);
    return failures ? 1 : 0;
}
