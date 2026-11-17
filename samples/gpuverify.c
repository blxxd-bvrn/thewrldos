/* Compare actual SVGA3D readback to the native CPU renderer at fixed poses.
 * Uses the same owned workload as d3d10cube, not a second hand-written shader. */
#define WinMain cube_interactive_main
#include "d3d10cube.c"
#undef WinMain
#include <stdlib.h>

typedef struct {
    ID3D10Device *device; IDXGISwapChain *chain;
    ID3D10RenderTargetView *color; ID3D10DepthStencilView *depth;
    ID3D10Buffer *vertex, *index, *constant;
} verify_state_t;
static int verify_checks, verify_failures;
static void bind_state(const verify_state_t *s) {
    dev=s->device;swap=s->chain;rtv=s->color;dsv=s->depth;vb=s->vertex;ib=s->index;cb=s->constant;
}
static void save_state(verify_state_t *s) {
    *s=(verify_state_t){dev,swap,rtv,dsv,vb,ib,cb};
}
static void verify_check(const char *name,int good) {
    verify_checks++;
    if(!good){char text[200];verify_failures++;sprintf(text,"GPUVERIFY FAIL %s",name);OutputDebugStringA(text);}
}
static void release_state(verify_state_t *s) {
    bind_state(s);
    if(dev)ID3D10Device_ClearState(dev);
    if(rtv)ID3D10RenderTargetView_Release(rtv);if(dsv)ID3D10DepthStencilView_Release(dsv);
    if(vb)ID3D10Buffer_Release(vb);if(ib)ID3D10Buffer_Release(ib);if(cb)ID3D10Buffer_Release(cb);
    if(swap)IDXGISwapChain_Release(swap);if(dev)ID3D10Device_Release(dev);
    memset(s,0,sizeof *s);
}
static int readback(unsigned char *out) {
    ID3D10Texture2D *back=NULL,*staging=NULL;
    HRESULT hr=IDXGISwapChain_GetBuffer(swap,0,&IID_ID3D10Texture2D,(void **)&back);
    if(FAILED(hr)||!back)return 0;
    D3D10_TEXTURE2D_DESC desc;ID3D10Texture2D_GetDesc(back,&desc);
    desc.Usage=D3D10_USAGE_STAGING;desc.BindFlags=0;desc.CPUAccessFlags=D3D10_CPU_ACCESS_READ;desc.MiscFlags=0;
    hr=ID3D10Device_CreateTexture2D(dev,&desc,NULL,&staging);
    if(SUCCEEDED(hr)) {
        ID3D10Device_CopyResource(dev,(ID3D10Resource *)staging,(ID3D10Resource *)back);
        D3D10_MAPPED_TEXTURE2D map;
        hr=ID3D10Texture2D_Map(staging,0,D3D10_MAP_READ,0,&map);
        if(SUCCEEDED(hr)){for(int y=0;y<height;y++)memcpy(out+y*width*4,(unsigned char *)map.pData+y*map.RowPitch,width*4);ID3D10Texture2D_Unmap(staging,0);}
    }
    if(staging)ID3D10Texture2D_Release(staging);ID3D10Texture2D_Release(back);
    return SUCCEEDED(hr);
}
int WINAPI WinMain(HINSTANCE instance,HINSTANCE previous,LPSTR cmd,int show) {
    (void)previous;(void)show;
    int display=strstr(cmd,"--show")!=NULL;
    width=160;height=120;
    WNDCLASSA wc={0};wc.lpfnWndProc=WndProc;wc.hInstance=instance;wc.hCursor=LoadCursor(NULL,IDC_ARROW);wc.lpszClassName="GpuVerify";RegisterClassA(&wc);
    RECT r={0,0,width,height};AdjustWindowRect(&r,WS_OVERLAPPED|WS_CAPTION,FALSE);
    HWND cpu_window=CreateWindowA("GpuVerify","GPU verification: CPU",WS_OVERLAPPED|WS_CAPTION,80,80,r.right-r.left,r.bottom-r.top,NULL,NULL,instance,NULL);
    HWND gpu_window=CreateWindowA("GpuVerify","GPU verification: SVGA3D",WS_OVERLAPPED|WS_CAPTION,280,80,r.right-r.left,r.bottom-r.top,NULL,NULL,instance,NULL);
    if(!cpu_window||!gpu_window)return 2;
    if(display){ShowWindow(cpu_window,SW_SHOW);ShowWindow(gpu_window,SW_SHOW);}
    unsigned char *cpu_image=malloc(width*height*4),*gpu_image=malloc(width*height*4);
    if(!cpu_image||!gpu_image)return 3;
    for(int model=4;model<=5;model++) {
        verify_state_t cpu={0},gpu={0};shader_model=model;
        dev=NULL;swap=NULL;rtv=NULL;dsv=NULL;vb=ib=cb=NULL;force_cpu=1;
        HRESULT hr=init(cpu_window);save_state(&cpu);verify_check("reference device initialization",SUCCEEDED(hr));if(FAILED(hr)){release_state(&cpu);break;}
        dev=NULL;swap=NULL;rtv=NULL;dsv=NULL;vb=ib=cb=NULL;force_cpu=0;
        hr=init(gpu_window);save_state(&gpu);verify_check("hardware device initialization",SUCCEEDED(hr));if(FAILED(hr)){release_state(&cpu);release_state(&gpu);break;}
        const float poses[]={.3f,.8f,1.4f,2.1f};
        for(unsigned pose=0;pose<sizeof poses/sizeof poses[0];pose++) {
            bind_state(&cpu);render(poses[pose]);verify_check("reference readback",readback(cpu_image));
            bind_state(&gpu);render(poses[pose]);verify_check("GPU readback",readback(gpu_image));
            UINT gpu_count=0,cpu_count=0;if(stats_fn)stats_fn(dev,&gpu_count,&cpu_count);
            verify_check("draw actually executed on GPU",gpu_count==pose+1&&cpu_count==0);
            unsigned long sum=0,bad=0;
            for(int i=0;i<width*height;i++){int max=0;for(int c=0;c<3;c++){int d=(int)cpu_image[i*4+c]-gpu_image[i*4+c];if(d<0)d=-d;sum+=d;if(d>max)max=d;}if(max>24)bad++;}
            unsigned long mean1000=(unsigned long)((unsigned long long)sum*1000/(width*height*3));
            char result[200];sprintf(result,"GPUVERIFY SM%d pose=%u mean_x1000=%lu bad_pixels=%lu/%d backend=%s",model,pose,mean1000,(unsigned long)bad,width*height,backend_name());OutputDebugStringA(result);
            verify_check("CPU/GPU image agreement",mean1000<=2000&&bad<=(unsigned)(width*height/50));
            if(display){MSG m;while(PeekMessageA(&m,NULL,0,0,PM_REMOVE)){TranslateMessage(&m);DispatchMessageA(&m);}Sleep(1500);}
        }
        release_state(&cpu);release_state(&gpu);
    }
    free(cpu_image);free(gpu_image);
    char result[160];sprintf(result,"GPUVERIFY %s %d checks",verify_failures?"FAILED":"passed",verify_checks);OutputDebugStringA(result);
    return verify_failures?1:0;
}
