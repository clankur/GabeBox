#include "gpu-receiver.h"
#include "frame-trace.h"
#include "../shared/sdl-host-protocol.h"
#include <dxgi1_2.h>
#include <appmodel.h>
#include <winstring.h>
#include <new>

extern "C" HRESULT FhdOpenTargets(void *, HRESULT (*)(const WCHAR *), ID3D11Device **, ID3D11DeviceContext **,
                                   ID3D11RenderTargetView **, IDXGISwapChain1 **);
extern "C" void FhdCloseTargets();
extern "C" HRESULT FhdLocalPath(WCHAR *,DWORD);
extern "C" int FhdWindowVisible(void *);
extern "C" LONG FhdReadActivation(void *);
extern "C" HRESULT FhdRequestReturn();
#ifdef FHD_NATIVE_INPUT
extern "C" HRESULT FhdBeginHostedInput(SdlHostMailbox *,void *);
extern "C" void FhdSetHostedInputProfile(BOOL,UINT,UINT);
extern "C" BOOL FhdHostedInputForeground();
extern "C" int FhdPollHostedInput(SdlHostMailbox *,void *);
extern "C" void FhdResumeHostedInput();
extern "C" void FhdEndHostedInput();
#endif
namespace {
SRWLOCK stateLock=SRWLOCK_INIT;
SdlHostMailbox *box;
HANDLE baseFile=INVALID_HANDLE_VALUE, child;
alignas(HostedGpuReceiver) BYTE receiverStorage[sizeof(HostedGpuReceiver)];
HostedGpuReceiver *receiver;
volatile LONG generation, starting, stop, running, overlay, submitted, sharedCount, cpuCount, width, height, visible, activation, gpuError, hardError;
const char * volatile gpuStage="idle";
volatile LONG64 sampleUs, sampleFrames;
UINT beginCount;
FhdFrameTrace trace;
struct Text {
    WCHAR bytes[768]{}; UINT length=0;
    void add(const WCHAR *s) {while (*s && length+1<ARRAYSIZE(bytes)) bytes[length++]=*s++;bytes[length]=0;}
    void ascii(const char *s) {while (*s && length+1<ARRAYSIZE(bytes)) bytes[length++]=(WCHAR)*s++;bytes[length]=0;}
    void num(UINT64 n) {if(n>=10)num(n/10);WCHAR s[]={(WCHAR)(L'0'+n%10),0};add(s);}
    void field(const WCHAR *key, UINT64 n) {add(key);num(n);add(L"\r\n");}
};
volatile LONG sharedMode=0, nativePad=1;
volatile LONG pointerSample=-1, pointerSerial, pointerAck, pointerEvents, pointerForeground;
void samplePointer() {
#ifdef FHD_NATIVE_INPUT
    pointerSample=nativePad || !box ? -1 : SdlHostLoad(&box->input_pointer);
    if (pointerSample >= 0) {
        pointerSerial=SdlHostLoad(&box->input_serial);
        pointerAck=SdlHostLoad(&box->input_ack);
        pointerEvents=SdlHostLoad(&box->input_events);
        pointerForeground=FhdHostedInputForeground();
    }
#endif
}
void report(Text &t) {
    t.add(L"mode=");t.add(sharedMode?L"shared-texture":L"cpu-upload");t.add(L"\r\n");
    t.field(L"presented=",submitted);t.field(L"gpu_presented=",sharedCount);t.field(L"cpu_presented=",cpuCount);
    t.field(L"width=",width);t.field(L"height=",height);t.field(L"visible=",visible);t.field(L"activation_mode=",activation);
    t.field(L"overlay_requested=",overlay);t.field(L"running=",running);
    t.field(L"sample_us=",InterlockedCompareExchange64(&sampleUs,0,0));
    t.field(L"sample_frames=",InterlockedCompareExchange64(&sampleFrames,0,0));
    t.add(L"gpu_stage=");t.ascii(gpuStage);t.add(L"\r\n");
    t.field(L"gpu_hresult=",static_cast<UINT>(gpuError));
    const LONG point=pointerSample;
    if (point >= 0) {
        t.field(L"pointer_x_4095=",point & 4095);t.field(L"pointer_y_4095=",(point >> 12) & 4095);
        t.field(L"pointer_down=",(point >> 24) & 1);t.field(L"pointer_foreground=",pointerForeground);
        t.field(L"pointer_serial=",pointerSerial);t.field(L"pointer_ack=",pointerAck);t.field(L"pointer_events=",pointerEvents);
    }
    if (hardError) t.field(L"error=",static_cast<UINT>(hardError));
}
void releaseBase() {
    if (receiver) {receiver->~HostedGpuReceiver();receiver=nullptr;}
    if (box) {UnmapViewOfFile(box);box=nullptr;}
    if (baseFile!=INVALID_HANDLE_VALUE){CloseHandle(baseFile);baseFile=INVALID_HANDLE_VALUE;}
    if (child){CloseHandle(child);child=nullptr;}
}
HRESULT attach(const WCHAR *path,bool create) {
    if (running) return E_PENDING;
    InterlockedIncrement(&generation);
    starting=0;
    releaseBase();
    CREATEFILE2_EXTENDED_PARAMETERS flags{};flags.dwSize=sizeof(flags);
    if(create) {flags.dwFileFlags=FILE_FLAG_DELETE_ON_CLOSE;flags.dwFileAttributes=FILE_ATTRIBUTE_TEMPORARY;}
    baseFile=CreateFile2(path,GENERIC_READ|GENERIC_WRITE|(create?DELETE:0),FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,
                        create?CREATE_NEW:OPEN_EXISTING,&flags);
    if (baseFile==INVALID_HANDLE_VALUE) return HRESULT_FROM_WIN32(GetLastError());
    LARGE_INTEGER size{};
    if (!create && (!GetFileSizeEx(baseFile,&size)||size.QuadPart<static_cast<LONGLONG>(sizeof(SdlHostMailbox)))) {releaseBase();return E_INVALIDARG;}
    HANDLE mapping=CreateFileMappingFromApp(baseFile,nullptr,PAGE_READWRITE,sizeof(SdlHostMailbox),nullptr);
    if (mapping) {box=(SdlHostMailbox*)MapViewOfFileFromApp(mapping,FILE_MAP_WRITE,0,sizeof(SdlHostMailbox));CloseHandle(mapping);}
    if(!box) {HRESULT hr=HRESULT_FROM_WIN32(GetLastError());releaseBase();return hr;}
    if(create) InterlockedExchange(&box->magic,SDL_HOST_MAGIC);
    if(SdlHostLoad(&box->magic)!=SDL_HOST_MAGIC) {releaseBase();return E_INVALIDARG;}
    receiver=new(receiverStorage) HostedGpuReceiver;
    HRESULT hr=receiver->Open(path);
    if (FAILED(hr)) {releaseBase();return hr;}
    stop=overlay=submitted=sharedCount=cpuCount=width=height=visible=activation=gpuError=hardError=sharedMode=0;
    pointerSample=-1;
    gpuStage="waiting-for-producer";sampleUs=sampleFrames=0;
    return S_OK;
}
LONG mediaToken;
HRESULT drawMailbox(ID3D11Device *device, ID3D11DeviceContext *ctx, ID3D11RenderTargetView *target,
                    bool nativeGamepad, UINT64 &sequence, bool &gpu) {
    const LONG pointer = nativeGamepad ? -1 : SdlHostLoad(&box->input_pointer);
    HRESULT hr = receiver->Draw(device, ctx, target, 1920, 1080, sequence, pointer);
    gpuStage = receiver->Stage();
    gpu = hr == S_OK;
    if (gpu) return S_OK;
    if (FAILED(hr)) gpuError = hr;
    if (receiver->Active() ||
        InterlockedCompareExchange(&box->frame_state, SDL_HOST_READING, SDL_HOST_READY) != SDL_HOST_READY) return S_FALSE;
    auto header = box->frame;
    hr = receiver->DrawCpu(device, ctx, target, 1920, 1080, box->pixels, header.width, header.height, pointer);
    InterlockedExchange(&box->frame_state, SDL_HOST_EMPTY);
    sequence = header.sequence;
    return hr;
}
HRESULT validate(HSTRING h,const WCHAR **out) {
    UINT32 n;const WCHAR* name=WindowsGetStringRawBuffer(h,&n);
    WCHAR local[1024];HRESULT hr=FhdLocalPath(local,ARRAYSIZE(local));if(FAILED(hr))return hr;
    UINT i=0;while(local[i] && i<n){if(local[i]!=name[i])return E_INVALIDARG;++i;}
    if(local[i] || n>1000 || i+15>=n || name[i++]!=L'\\')return E_INVALIDARG;
    const WCHAR prefix[]=L"sdl-host-";for(UINT j=0;prefix[j];j++,i++)if(name[i]!=prefix[j])return E_INVALIDARG;
    for(;i+4<n;i++) if ((name[i]<L'0'||name[i]>L'9')&&name[i]!=L'-')return E_INVALIDARG;
    const WCHAR suffix[]=L".bin";for(UINT j=0;j<4;j++)if(name[i+j]!=suffix[j])return E_INVALIDARG;
    *out=name;return S_OK;
}
}
extern "C" HRESULT FhdAttach(HSTRING h) {
    const WCHAR *path=nullptr;HRESULT hr=validate(h,&path);if(FAILED(hr))return hr;
    AcquireSRWLockExclusive(&stateLock);hr=attach(path,false);ReleaseSRWLockExclusive(&stateLock);return hr;
}
extern "C" HRESULT FhdClaimMedia(HSTRING h, LONG *token) {
    const WCHAR *path = nullptr;
    HRESULT hr = validate(h, &path);
    if (FAILED(hr)) return hr;
    AcquireSRWLockExclusive(&stateLock);
    hr = (running || starting || (receiver && !stop)) ? E_PENDING : attach(path, false);
    if (SUCCEEDED(hr)) { mediaToken = generation; *token = mediaToken; running = 1; }
    ReleaseSRWLockExclusive(&stateLock);
    return hr;
}
extern "C" void FhdReleaseMedia(LONG token) {
    AcquireSRWLockExclusive(&stateLock);
    if (token && mediaToken == token) {
        mediaToken = 0; running = 0; stop = 1;
        releaseBase();
    }
    ReleaseSRWLockExclusive(&stateLock);
}
extern "C" SdlHostMailbox *FhdMediaInputMailbox(LONG token) {
    AcquireSRWLockShared(&stateLock);
    auto mailbox = token && mediaToken == token ? box : nullptr;
    ReleaseSRWLockShared(&stateLock);
    return mailbox;
}
extern "C" void FhdMediaSequences(LONG token, UINT64& cpu, UINT64& gpu) {
    AcquireSRWLockShared(&stateLock);
    if (token && mediaToken == token && box && receiver) {
        cpu = static_cast<UINT32>(SdlHostLoad(&box->frame_sequence));
        gpu = receiver->ProducerSequence();
    }
    ReleaseSRWLockShared(&stateLock);
}
extern "C" HRESULT FhdDrawMedia(LONG token, ID3D11Device *dev, ID3D11DeviceContext *ctx, ID3D11RenderTargetView *target,
                                bool nativePad, UINT64 &sequence, bool &gpu) {
    AcquireSRWLockShared(&stateLock);
    HRESULT hr = (mediaToken == token && box) ?
        drawMailbox(dev,ctx,target,nativePad,sequence,gpu) : RO_E_CLOSED;
    ReleaseSRWLockShared(&stateLock);
    return hr;
}
extern "C" HRESULT FhdPrepareStart(LONG *token) {
    AcquireSRWLockExclusive(&stateLock);
    HRESULT hr = E_UNEXPECTED;
    if (receiver && !stop && !mediaToken) {
        InterlockedExchange(&overlay,0);
        hr = running || starting ? S_FALSE : S_OK;
        starting = 1;
        *token = generation;
    }
    ReleaseSRWLockExclusive(&stateLock);
    return hr;
}
extern "C" bool FhdCurrentSession(LONG token) {
    return token==InterlockedCompareExchange(&generation,0,0) && !InterlockedCompareExchange(&stop,0,0);
}
extern "C" void FhdViewFailed(LONG token,HRESULT hr) {
    bool current=false;
    AcquireSRWLockExclusive(&stateLock);
    if (token==generation && !stop) {
        hardError=hr;
        current=true;
    }
    ReleaseSRWLockExclusive(&stateLock);
    if (current) FhdRequestReturn();
}
extern "C" void FhdNativePad(BYTE v) {InterlockedExchange(&nativePad,v?1:0);}
extern "C" void FhdCloseSession() {
    AcquireSRWLockExclusive(&stateLock);
    if (mediaToken) { ReleaseSRWLockExclusive(&stateLock); return; }
    InterlockedExchange(&stop,1);
    starting=0;
    if (!running) releaseBase();
    ReleaseSRWLockExclusive(&stateLock);
}
extern "C" HRESULT FhdSessionStatus(HSTRING *s) {Text t;report(t);return WindowsCreateString(t.bytes,t.length,s);}
extern "C" HRESULT FhdLaunchTest() {
#ifndef FHD_CONTROL_CHILD
    return E_NOTIMPL;
#else
    WCHAR local[1024];HRESULT hr=FhdLocalPath(local,ARRAYSIZE(local));if(FAILED(hr))return hr;
    Text path;path.add(local);path.add(L"\\sdl-host-");path.num(GetCurrentProcessId());path.add(L"-");path.num(GetTickCount64());path.add(L".bin");
    AcquireSRWLockExclusive(&stateLock);hr=attach(path.bytes,true);
    if(SUCCEEDED(hr)) {
        UINT n=1024;WCHAR package[1024];LONG err=GetCurrentPackagePath(&n,package);
        if(err!=ERROR_SUCCESS)hr=HRESULT_FROM_WIN32(err);
        else {
            Text exe;exe.add(package);exe.add(L"\\sdl-gpu-control.exe");
            Text trace;trace.add(local);trace.add(L"\\fhd-producer.txt");
            WCHAR oldMailbox[1024],oldTrace[1024];
            DWORD m=GetEnvironmentVariableW(L"XBOX_SDL_HOST_MAILBOX",oldMailbox,ARRAYSIZE(oldMailbox));
            DWORD l=GetEnvironmentVariableW(L"XBOX_SDL_TRACE_FILE",oldTrace,ARRAYSIZE(oldTrace));
            SetEnvironmentVariableW(L"XBOX_SDL_HOST_MAILBOX",path.bytes);SetEnvironmentVariableW(L"XBOX_SDL_TRACE_FILE",trace.bytes);
            STARTUPINFOW si{};si.cb=sizeof(si);PROCESS_INFORMATION pi{};
            if (CreateProcessW(exe.bytes,nullptr,nullptr,nullptr,FALSE,0,nullptr,package,&si,&pi)) {CloseHandle(pi.hThread);child=pi.hProcess;}
            else hr=HRESULT_FROM_WIN32(GetLastError());
            SetEnvironmentVariableW(L"XBOX_SDL_HOST_MAILBOX",m&&m<ARRAYSIZE(oldMailbox)?oldMailbox:nullptr);
            SetEnvironmentVariableW(L"XBOX_SDL_TRACE_FILE",l&&l<ARRAYSIZE(oldTrace)?oldTrace:nullptr);
        }
    }
    if(FAILED(hr)) releaseBase();
    ReleaseSRWLockExclusive(&stateLock);return hr;
#endif
}

extern "C" HRESULT FhdHostedLoop(LONG token,void *window,void *dispatcher,HRESULT (*pump)(void*),HRESULT (*save)(const WCHAR*)) {
    AcquireSRWLockExclusive(&stateLock);
    if (token!=generation || !receiver || stop || running) {ReleaseSRWLockExclusive(&stateLock);return E_UNEXPECTED;}
    InterlockedExchange(&running,1);ReleaseSRWLockExclusive(&stateLock);
    ID3D11Device *device=nullptr; ID3D11DeviceContext *ctx=nullptr;ID3D11RenderTargetView *target=nullptr;IDXGISwapChain1 *chain=nullptr;
    HRESULT hr=FhdOpenTargets(window,save,&device,&ctx,&target,&chain);
    LARGE_INTEGER freq{},now{};QueryPerformanceFrequency(&freq);
    LONGLONG firstTick=0,sampleStart=0;
    HANDLE wait=CreateEventExW(nullptr,nullptr,0,SYNCHRONIZE);
    ULONGLONG lastReport=GetTickCount64();
    trace.open();
#ifdef FHD_NATIVE_INPUT
    bool inputPaused=false;
    if(SUCCEEDED(hr))hr=FhdBeginHostedInput(box,window);
#endif
    while(SUCCEEDED(hr)&&!InterlockedCompareExchange(&stop,0,0)&&!SdlHostLoad(&box->quit)) {
        hr=pump(dispatcher);if(FAILED(hr))break;
#ifdef FHD_NATIVE_INPUT
        FhdSetHostedInputProfile(nativePad,width?width:1920,height?height:1080);
        if (inputPaused && !overlay) {FhdResumeHostedInput();inputPaused=false;}
        if (!overlay && FhdPollHostedInput(box,window)) {
            if (SUCCEEDED(FhdRequestReturn())) {InterlockedExchange(&overlay,1);inputPaused=true;}
            else FhdResumeHostedInput();
        }
#endif
        if (overlay) {if (wait) WaitForSingleObjectEx(wait,15,FALSE);continue;}
        UINT64 seq = 0;
        bool gpu = false;
        hr = drawMailbox(device,ctx,target,nativePad != 0,seq,gpu);
        if (FAILED(hr)) break;
        if(hr==S_OK) {
            hr=chain->Present(1,0);if(FAILED(hr))break;
            if(hr==S_OK) {
                ++submitted;if(gpu)++sharedCount;else ++cpuCount;
                sharedMode=gpu?1:0;width=receiver->SourceWidth();height=receiver->SourceHeight();
                QueryPerformanceCounter(&now);
                trace.record(now.QuadPart,seq,submitted,gpu);
                if(!firstTick)firstTick=now.QuadPart;
                if(!sampleStart && now.QuadPart-firstTick>=5*freq.QuadPart){sampleStart=now.QuadPart;beginCount=submitted;}
                if(sampleStart && !sampleUs && now.QuadPart-sampleStart>=30*freq.QuadPart){
                    InterlockedExchange64(&sampleUs,(now.QuadPart-sampleStart)*1000000/freq.QuadPart);
                    InterlockedExchange64(&sampleFrames,submitted-beginCount);
                }
            }
        } else if(wait) WaitForSingleObjectEx(wait,1,FALSE);
        auto ms=GetTickCount64();
        if(ms-lastReport>3000) {
            samplePointer();
            visible=FhdWindowVisible(window);activation=FhdReadActivation(window);
            if (trace.file!=INVALID_HANDLE_VALUE) {trace.flush();Text t;report(t);save(t.bytes);}
            lastReport=ms;
        }
        if (child && WaitForSingleObjectEx(child,0,FALSE)==WAIT_OBJECT_0)break;
    }
#ifdef FHD_NATIVE_INPUT
    samplePointer();
    FhdEndHostedInput();
#endif
    trace.close();
    if(wait)CloseHandle(wait);
    if(FAILED(hr))hardError=hr;
    AcquireSRWLockExclusive(&stateLock);releaseBase();FhdCloseTargets();starting=0;running=0;ReleaseSRWLockExclusive(&stateLock);
    Text t;report(t);save(t.bytes);return hr;
}
