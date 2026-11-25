#define WIN32_LEAN_AND_MEAN
#define COBJMACROS
#define INITGUID
#include <windows.h>
#include <d3d11_1.h>
#include <dxgi1_2.h>

typedef HRESULT (*ProcessPending)(void *);
typedef HRESULT (*SaveReport)(const WCHAR *);

static ID3D11Device *device;
static ID3D11DeviceContext *context;
static ID3D11DeviceContext1 *context1;
static IDXGISwapChain1 *chain;
static ID3D11RenderTargetView *target;

static WCHAR *add(WCHAR *p, const WCHAR *s) { while (*s) *p++ = *s++; *p=0; return p; }
static WCHAR *decimal(WCHAR *p, UINT64 value) {
    if (value >= 10) p = decimal(p, value/10);
    *p++ = (WCHAR)('0' + value % 10); *p=0; return p;
}
static HRESULT failure(SaveReport save, const WCHAR *stage, HRESULT hr) {
    WCHAR text[180]; WCHAR *p=add(text,L"status=fhd-error\r\nstage=");
    p=add(p,stage); p=add(p,L"\r\nhresult=0x");
    for (int i=28;i>=0;i-=4) *p++=L"0123456789ABCDEF"[((UINT)hr>>i)&15];
    add(p,L"\r\n"); save(text); return hr;
}
static void releaseObjects(void) {
    if (target) { ID3D11RenderTargetView_Release(target); target=NULL; }
    if (chain) { IDXGISwapChain1_Release(chain); chain=NULL; }
    if (context1) { ID3D11DeviceContext1_Release(context1); context1=NULL; }
    if (context) { ID3D11DeviceContext_Release(context); context=NULL; }
    if (device) { ID3D11Device_Release(device); device=NULL; }
}

static HRESULT initialize(IUnknown *window, SaveReport save, BOOL tryForeground) {
    HRESULT hr; D3D_FEATURE_LEVEL level;
    IDXGIDevice *dxgi=NULL; IDXGIAdapter *adapter=NULL; IDXGIFactory2 *factory=NULL;
    ID3D11Texture2D *back=NULL;
    DXGI_SWAP_CHAIN_DESC1 desc={0}; const WCHAR *stage=L"d3d11-create-device";
    hr=D3D11CreateDevice(NULL,D3D_DRIVER_TYPE_HARDWARE,NULL,D3D11_CREATE_DEVICE_BGRA_SUPPORT,
                         NULL,0,D3D11_SDK_VERSION,&device,&level,&context);
    if (FAILED(hr)) goto end;
    stage=L"dxgi-device"; hr=ID3D11Device_QueryInterface(device,&IID_IDXGIDevice,(void**)&dxgi); if (FAILED(hr)) goto end;
    stage=L"dxgi-adapter"; hr=IDXGIDevice_GetAdapter(dxgi,&adapter); if (FAILED(hr)) goto end;
    stage=L"dxgi-factory"; hr=IDXGIAdapter_GetParent(adapter,&IID_IDXGIFactory2,(void**)&factory); if (FAILED(hr)) goto end;
    stage=L"device-context1"; hr=ID3D11DeviceContext_QueryInterface(context,&IID_ID3D11DeviceContext1,(void**)&context1);
    if (FAILED(hr)) goto end;
    desc.Width=1920; desc.Height=1080; desc.Format=DXGI_FORMAT_B8G8R8A8_UNORM;
    desc.SampleDesc.Count=1; desc.BufferUsage=DXGI_USAGE_RENDER_TARGET_OUTPUT;
    desc.BufferCount=2; desc.Scaling=DXGI_SCALING_STRETCH;
    desc.SwapEffect=DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL; desc.AlphaMode=DXGI_ALPHA_MODE_IGNORE;
    stage=L"swap-chain-core-window";
    hr=IDXGIFactory2_CreateSwapChainForCoreWindow(factory,(IUnknown*)device,window,&desc,NULL,&chain);
    if (FAILED(hr) && tryForeground) {
        failure(save,stage,hr);
        stage=L"swap-chain-foreground";
        desc.AlphaMode=DXGI_ALPHA_MODE_PREMULTIPLIED;
        desc.Scaling=DXGI_SCALING_NONE;
        desc.Flags=DXGI_SWAP_CHAIN_FLAG_FOREGROUND_LAYER;
        hr=IDXGIFactory2_CreateSwapChainForCoreWindow(factory,(IUnknown*)device,window,&desc,NULL,&chain);
    }
    if (FAILED(hr)) goto end;
    stage=L"swap-chain-buffer"; hr=IDXGISwapChain1_GetBuffer(chain,0,&IID_ID3D11Texture2D,(void**)&back); if (FAILED(hr)) goto end;
    stage=L"create-target"; hr=ID3D11Device_CreateRenderTargetView(device,(ID3D11Resource*)back,NULL,&target);
end:
    if (back) ID3D11Texture2D_Release(back);
    if (factory) IDXGIFactory2_Release(factory);
    if (adapter) IDXGIAdapter_Release(adapter);
    if (dxgi) IDXGIDevice_Release(dxgi);
    if (FAILED(hr)) { releaseObjects(); return failure(save,stage,hr); }
    return S_OK;
}

static void rect(int x, int y, int w, int h, const FLOAT color[4]) {
    const D3D11_RECT box={x,y,x+w,y+h};
    ID3D11DeviceContext1_ClearView(context1,(ID3D11View*)target,color,&box,1);
}
static UINT calibration=0;
static void readCalibration(void) {
    ID3D11Texture2D *src=NULL,*stage=NULL;
    D3D11_TEXTURE2D_DESC desc={0};desc.Width=desc.Height=desc.MipLevels=desc.ArraySize=desc.SampleDesc.Count=1;
    desc.Format=DXGI_FORMAT_B8G8R8A8_UNORM;desc.Usage=D3D11_USAGE_STAGING;desc.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
    if (FAILED(IDXGISwapChain1_GetBuffer(chain,0,&IID_ID3D11Texture2D,(void**)&src))) return;
    if (SUCCEEDED(ID3D11Device_CreateTexture2D(device,&desc,NULL,&stage))) {
        D3D11_BOX box={82,106,0,83,107,1};D3D11_MAPPED_SUBRESOURCE read;
        ID3D11DeviceContext_CopySubresourceRegion(context,(ID3D11Resource*)stage,0,0,0,0,(ID3D11Resource*)src,0,&box);
        if (SUCCEEDED(ID3D11DeviceContext_Map(context,(ID3D11Resource*)stage,0,D3D11_MAP_READ,0,&read))) {
            calibration=*(const UINT*)read.pData;ID3D11DeviceContext_Unmap(context,(ID3D11Resource*)stage,0);
        }
        ID3D11Texture2D_Release(stage);
    }
    ID3D11Texture2D_Release(src);
}
static HRESULT paint(UINT sequence, UINT sync) {
    const FLOAT navy[]={0.018f,0.031f,0.075f,1}, teal[]={0.08f,0.84f,0.77f,1};
    const FLOAT gold[]={1,0.68f,0.12f,1}, white[]={0.92f,0.95f,1,1};
    ID3D11DeviceContext_ClearRenderTargetView(context,target,navy);
    rect(80,106,1760,2,white); rect(80,970,1760,2,white);
    rect(80+(int)(sequence*4%1700),141,36,793,teal);
    for (int bit=0;bit<16;bit++) if (sequence&(1u<<bit)) rect(92+bit*46,45,31,36,gold);
    for (int x=0;x<192;x+=2) rect(1536+x,994,1,45,white);
    if (sequence==8) readCalibration();
    return IDXGISwapChain1_Present(chain,sync,0);
}

extern int FhdWindowVisible(void *);
#ifdef FHD_SECONDARY_VIEW
extern LONG FhdReadActivation(void *);
extern LONG FhdCurrentApplicationViewId(void);
#endif
HRESULT FhdPresent(void *window, void *dispatcher, ProcessPending pump, SaveReport save) {
    HRESULT hr=initialize((IUnknown*)window,save,FALSE);
    if (FAILED(hr)) return hr;
    save(L"status=fhd-ready\r\nwidth=1920\r\nheight=1080\r\n");
    LARGE_INTEGER freq,start,now,first={0}; QueryPerformanceFrequency(&freq); QueryPerformanceCounter(&start);
    UINT sequence=0,samples=0; BOOL reported=FALSE; UINT firstDisplayed=0;
    DXGI_FRAME_STATISTICS stats={0}; BOOL statsAtStart=FALSE;
    while (1) {
        hr=pump(dispatcher); if (FAILED(hr)) { failure(save,L"dispatcher",hr); break; }
        hr=paint(++sequence,1); if (hr!=S_OK) { failure(save,L"present",hr); break; }
        QueryPerformanceCounter(&now);
        if (!first.QuadPart && now.QuadPart-start.QuadPart>=5*freq.QuadPart) {
            first=now;
            statsAtStart=SUCCEEDED(IDXGISwapChain1_GetFrameStatistics(chain,&stats));
            firstDisplayed=stats.PresentCount;
        } else if (first.QuadPart && !reported) {
            ++samples;
            if (now.QuadPart-first.QuadPart >= 30*freq.QuadPart) {
                WCHAR text[512]; WCHAR *p=add(text,L"status=fhd-complete\r\nwidth=1920\r\nheight=1080\r\nunique_present_calls=");
                p=decimal(p,samples); p=add(p,L"\r\nsample_us=");
                p=decimal(p,(UINT64)(now.QuadPart-first.QuadPart)*1000000u/(UINT64)freq.QuadPart);
                hr=IDXGISwapChain1_GetFrameStatistics(chain,&stats); p=add(p,L"\r\ndxgi_displayed=");
                if (SUCCEEDED(hr)&&statsAtStart) p=decimal(p,(UINT)(stats.PresentCount-firstDisplayed));
                else p=add(p,L"unavailable");
                p=add(p,L"\r\nvisible=");p=decimal(p,(UINT)FhdWindowVisible(window));
#ifdef FHD_SECONDARY_VIEW
                p=add(p,L"\r\nactivation_mode=");p=decimal(p,(UINT)FhdReadActivation(window));
                p=add(p,L"\r\napplication_view_id_u32=");p=decimal(p,(UINT)FhdCurrentApplicationViewId());
#endif
                UINT last=0;IDXGISwapChain1_GetLastPresentCount(chain,&last);
                p=add(p,L"\r\nlast_present_count=");p=decimal(p,last);
                p=add(p,L"\r\ncalibration_bgra=");p=decimal(p,calibration);
                add(p,L"\r\n"); save(text); reported=TRUE;
            }
        }
        if (now.QuadPart-start.QuadPart>120*freq.QuadPart) break;
    }
    releaseObjects(); return hr;
}


// WWA invokes once per animation callback. Pixels stay on the DXGI device.
HRESULT FhdBeginWwa(IUnknown *window, SaveReport save) { return initialize(window,save,TRUE); }
HRESULT FhdDrawWwa(UINT sequence) { return paint(sequence,0); }
HRESULT FhdCountWwa(UINT *displayed) {
    DXGI_FRAME_STATISTICS stats={0};
    HRESULT hr=IDXGISwapChain1_GetFrameStatistics(chain,&stats);
    if (SUCCEEDED(hr)) *displayed=stats.PresentCount;
    return hr;
}

HRESULT FhdOpenTargets(void *window, SaveReport save, ID3D11Device **d, ID3D11DeviceContext **c,
                       ID3D11RenderTargetView **t, IDXGISwapChain1 **s) {
    HRESULT hr=initialize((IUnknown*)window,save,FALSE);
    if (SUCCEEDED(hr)) {*d=device;*c=context;*t=target;*s=chain;}
    return hr;
}
void FhdCloseTargets(void) {releaseObjects();}
