#include <windows.h>
#include <roapi.h>
#include <activation.h>
#include <winstring.h>
#ifdef FHD_SDK_METADATA
#include <winrt/Fhd.Native.h>
#endif
#ifdef FHD_SDK_METADATA
extern "C" HRESULT FhdOpenMedia(HSTRING, BYTE, void**);
extern "C" HRESULT FhdMediaStatus(HSTRING*);
extern "C" HRESULT FhdCloseMedia();
extern "C" HRESULT FhdOpenPanel(HSTRING, BYTE, void**);
extern "C" HRESULT FhdSetPanelVisible(BYTE, void**);
extern "C" HRESULT FhdPanelStatus(HSTRING*);
extern "C" HRESULT FhdClosePanel(void**);
#endif
extern "C" HRESULT FhdAttach(HSTRING);
extern "C" HRESULT FhdPrepareStart(LONG*);
extern "C" bool FhdCurrentSession(LONG);
extern "C" HRESULT FhdRequestReturn();
extern "C" HRESULT FhdSessionStatus(HSTRING *);
extern "C" void FhdCloseSession();
extern "C" HRESULT FhdLaunchTest();
extern "C" void FhdNativePad(BYTE);
extern "C" void *FhdNativeViewSource(LONG);
namespace {
const WCHAR klass[]=L"Fhd.Native.RenderHost";
#ifdef FHD_SDK_METADATA
const GUID &iidStatics = reinterpret_cast<GUID const&>(winrt::guid_of<winrt::Fhd::Native::IRenderHostStatics>());
#else
const GUID iidStatics={0xcc64f318,0x7fd5,0x4d27,{0x99,0x70,0xc3,0xd6,0xb6,0x29,0x9d,0x5b}};
#endif
const GUID iidActivation={0x35,0,0,{0xc0,0,0,0,0,0,0,0x46}};
const GUID iidInspectable={0xaf86e2e0,0xb12d,0x4c6a,{0x9c,0x5a,0xd7,0xaa,0x65,0x10,0x1e,0x90}};
const GUID iidImmersive={0x34a05b2f,0xee0d,0x41e5,{0x83,0x14,0xcf,0x10,0xc9,0x1b,0xf0,0xaf}};
const GUID iidInterop={0xc446fb5d,0x4793,0x4896,{0xa8,0xe2,0xbe,0x57,0xa8,0xbb,0x0f,0x50}};
const GUID iidCore5={0x4b4ae1e1,0x2e6d,0x4eaa,{0xbd,0xa1,0x1c,0x5c,0xc1,0xbe,0xe1,0x41}};
const GUID iidCoreStatic={0x4d239005,0x3c2a,0x41b1,{0x90,0x22,0x53,0x6b,0xb9,0xcf,0x93,0xb1}};
const GUID iidSwitch={0x975f2f1e,0xe656,0x4c5e,{0xa0,0xa1,0x71,0x7c,0x6f,0xfa,0x7d,0x64}};
struct Immersive : IInspectable {virtual HRESULT STDMETHODCALLTYPE CreateNewView(IInspectable *,IInspectable **)=0;};
struct Interop : IInspectable {virtual HRESULT STDMETHODCALLTYPE GetApplicationViewIdForWindow(IInspectable *,LONG*)=0;};
struct Core5 : IInspectable {virtual HRESULT STDMETHODCALLTYPE Queue(IInspectable**)=0;virtual HRESULT STDMETHODCALLTYPE Mode(LONG*)=0;};
struct CoreStatics : IInspectable {virtual HRESULT STDMETHODCALLTYPE Current(IInspectable**)=0;};
struct Switcher : IInspectable {
    virtual HRESULT STDMETHODCALLTYPE Disable()=0;
    virtual HRESULT STDMETHODCALLTYPE Standalone(LONG,IInspectable**)=0;
    virtual HRESULT STDMETHODCALLTYPE Standalone2(LONG,int,IInspectable**)=0;
    virtual HRESULT STDMETHODCALLTYPE Standalone3(LONG,int,LONG,int,IInspectable**)=0;
    virtual HRESULT STDMETHODCALLTYPE Switch(LONG,IInspectable**)=0;
    virtual HRESULT STDMETHODCALLTYPE SwitchFrom(LONG,LONG,IInspectable**)=0;
};
struct Statics : IInspectable {
    virtual HRESULT STDMETHODCALLTYPE Open(HSTRING)=0;
    virtual HRESULT STDMETHODCALLTYPE Start()=0;
    virtual HRESULT STDMETHODCALLTYPE ViewId(LONG*)=0;
    virtual HRESULT STDMETHODCALLTYPE Status(HSTRING*)=0;
    virtual HRESULT STDMETHODCALLTYPE Close()=0;
    virtual HRESULT STDMETHODCALLTYPE LaunchControl()=0;
    virtual HRESULT STDMETHODCALLTYPE SetNativeGamepad(BYTE)=0;
#ifdef FHD_SDK_METADATA
    virtual HRESULT STDMETHODCALLTYPE OpenMedia(HSTRING, BYTE, void**)=0;
    virtual HRESULT STDMETHODCALLTYPE MediaStatus(HSTRING*)=0;
    virtual HRESULT STDMETHODCALLTYPE CloseMedia()=0;
    virtual HRESULT STDMETHODCALLTYPE OpenPanel(HSTRING, BYTE, void**)=0;
    virtual HRESULT STDMETHODCALLTYPE SetPanelVisible(BYTE, void**)=0;
    virtual HRESULT STDMETHODCALLTYPE PanelStatus(HSTRING*)=0;
    virtual HRESULT STDMETHODCALLTYPE ClosePanel(void**)=0;
#endif
};
volatile LONG secondaryId=0,primaryId=0,windowError=0;
IInspectable *secondaryView;
template<typename T, size_t N> HRESULT get(const WCHAR (&name)[N],REFIID id,T **out) {
    HSTRING label=nullptr;HRESULT hr=WindowsCreateString(name,(UINT)(N-1),&label);
    if (SUCCEEDED(hr))hr=RoGetActivationFactory(label,id,(void**)out);
    WindowsDeleteString(label);return hr;
}
HRESULT identify(IInspectable *window,LONG &id) {
    Interop *a=nullptr;HRESULT hr=get(L"Windows.UI.ViewManagement.ApplicationView",iidInterop,&a);
    if(SUCCEEDED(hr)){hr=a->GetApplicationViewIdForWindow(window,&id);a->Release();}return hr;
}
class Factory final : public IActivationFactory, public Statics {
public:
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID id,void **out) override {
        if (!out)return E_POINTER;*out=nullptr;
        if(IsEqualIID(id,IID_IUnknown)||IsEqualIID(id,iidInspectable)||IsEqualIID(id,iidActivation)||IsEqualIID(id,IID_IAgileObject))
            *out=static_cast<IActivationFactory*>(this);
        else if(IsEqualIID(id,iidStatics))*out=static_cast<Statics*>(this);
        else return E_NOINTERFACE;
        return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef() override {return 2;} ULONG STDMETHODCALLTYPE Release() override {return 1;}
    HRESULT STDMETHODCALLTYPE GetIids(ULONG *n,IID **ids) override {
        if(!n||!ids)return E_POINTER;*ids=(IID*)CoTaskMemAlloc(sizeof(GUID));if(!*ids)return E_OUTOFMEMORY;**ids=iidStatics;*n=1;return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetRuntimeClassName(HSTRING *n) override {return WindowsCreateString(klass,ARRAYSIZE(klass)-1,n);}
    HRESULT STDMETHODCALLTYPE GetTrustLevel(TrustLevel *t) override {*t=BaseTrust;return S_OK;}
    HRESULT STDMETHODCALLTYPE ActivateInstance(IInspectable **out) override {*out=nullptr;return E_NOTIMPL;}
    HRESULT STDMETHODCALLTYPE Open(HSTRING p) override {return FhdAttach(p);}
    HRESULT STDMETHODCALLTYPE Start() override {
        LONG token=0;HRESULT hr=FhdPrepareStart(&token);if (hr!=S_OK)return hr;
        CoreStatics *core=nullptr;IInspectable *window=nullptr;LONG main=0;
        hr=get(L"Windows.UI.Core.CoreWindow",iidCoreStatic,&core);
        if(SUCCEEDED(hr)){hr=core->Current(&window);core->Release();}
        if(SUCCEEDED(hr)&&window){hr=identify(window,main);window->Release();}
        if(FAILED(hr)||!main){FhdCloseSession();return FAILED(hr)?hr:E_POINTER;}
        InterlockedExchange(&primaryId,main);InterlockedExchange(&secondaryId,0);InterlockedExchange(&windowError,0);
        if(secondaryView){secondaryView->Release();secondaryView=nullptr;}
        Immersive *app=nullptr;hr=get(L"Windows.ApplicationModel.Core.CoreApplication",iidImmersive,&app);
        if(SUCCEEDED(hr)){
            IInspectable *source=static_cast<IInspectable*>(FhdNativeViewSource(token));
            hr=source ? app->CreateNewView(source,&secondaryView) : E_OUTOFMEMORY;
            if(source)source->Release();
            app->Release();
        }
        if(FAILED(hr))FhdCloseSession();
        return hr;
    }
    HRESULT STDMETHODCALLTYPE ViewId(LONG *v) override {
        if(!v)return E_POINTER;*v=InterlockedCompareExchange(&secondaryId,0,0);return InterlockedCompareExchange(&windowError,0,0);
    }
    HRESULT STDMETHODCALLTYPE Status(HSTRING *s) override {return FhdSessionStatus(s);}
    HRESULT STDMETHODCALLTYPE Close() override {FhdCloseSession();FhdRequestReturn();return S_OK;}
    HRESULT STDMETHODCALLTYPE LaunchControl() override {return FhdLaunchTest();}
    HRESULT STDMETHODCALLTYPE SetNativeGamepad(BYTE b) override {FhdNativePad(b);return S_OK;}
#ifdef FHD_SDK_METADATA
    HRESULT STDMETHODCALLTYPE OpenMedia(HSTRING path, BYTE nativePad, void **out) override {return FhdOpenMedia(path,nativePad,out);}
    HRESULT STDMETHODCALLTYPE MediaStatus(HSTRING *out) override {return FhdMediaStatus(out);}
    HRESULT STDMETHODCALLTYPE CloseMedia() override {return FhdCloseMedia();}
    HRESULT STDMETHODCALLTYPE OpenPanel(HSTRING path, BYTE pad, void** out) override {return FhdOpenPanel(path,pad,out);}
    HRESULT STDMETHODCALLTYPE SetPanelVisible(BYTE visible, void** out) override {return FhdSetPanelVisible(visible,out);}
    HRESULT STDMETHODCALLTYPE PanelStatus(HSTRING* out) override {return FhdPanelStatus(out);}
    HRESULT STDMETHODCALLTYPE ClosePanel(void** out) override {return FhdClosePanel(out);}
#endif
};
}
extern "C" HRESULT FhdSecondaryWindowReady(void *window,LONG token) {
    if(!FhdCurrentSession(token))return E_ABORT;
    LONG id=0;HRESULT hr=identify((IInspectable*)window,id);
    if(FAILED(hr))InterlockedExchange(&windowError,hr);else InterlockedExchange(&secondaryId,id);
    return hr;
}
extern "C" LONG FhdReadActivation(void *window) {
    Core5 *v=nullptr;LONG mode=-99;if(SUCCEEDED(((IInspectable*)window)->QueryInterface(iidCore5,(void**)&v))){v->Mode(&mode);v->Release();}return mode;
}
extern "C" HRESULT FhdRequestReturn() {
    Switcher *s=nullptr;HRESULT hr=get(L"Windows.UI.ViewManagement.ApplicationViewSwitcher",iidSwitch,&s);
    if(SUCCEEDED(hr)){
        IInspectable *action=nullptr;
        LONG main=InterlockedCompareExchange(&primaryId,0,0), secondary=InterlockedCompareExchange(&secondaryId,0,0);
        hr=main && secondary ? s->SwitchFrom(main,secondary,&action) : S_FALSE;
        if(FAILED(hr)){
            if(action){action->Release();action=nullptr;}
            hr=s->Switch(main,&action);
        }
        s->Release();
        if(action)action->Release();
    }
    return hr;
}
extern "C" HRESULT WINAPI DllGetActivationFactory(HSTRING h,IActivationFactory **out) {
    if(!out)return E_POINTER;*out=nullptr;UINT len=0;const WCHAR *s=WindowsGetStringRawBuffer(h,&len);
    if(len!=ARRAYSIZE(klass)-1)return CLASS_E_CLASSNOTAVAILABLE;
    for(UINT i=0;i<len;i++)if(s[i]!=klass[i])return CLASS_E_CLASSNOTAVAILABLE;
    static Factory factory;*out=static_cast<IActivationFactory*>(&factory);return S_OK;
}
extern "C" HRESULT WINAPI DllCanUnloadNow() {return S_FALSE;}
