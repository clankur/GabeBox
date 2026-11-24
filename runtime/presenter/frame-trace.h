#pragma once
#include <windows.h>
#include <appmodel.h>

extern "C" HRESULT FhdLocalPath(WCHAR *,DWORD);

// Enabled by a test package's fhd-measure.txt. One row per successfully
// submitted, distinct producer sequence. No pixel data or controller input is recorded.
struct FhdFrameTrace {
    struct Header { UINT32 magic, version; UINT64 frequency, utcFileTime, counter; };
    struct Row { UINT64 counter, sequence; UINT32 presented, mode; };
    static_assert(sizeof(Header)==32 && sizeof(Row)==24,"FHD trace ABI");
    HANDLE file=INVALID_HANDLE_VALUE; LARGE_INTEGER frequency{};
    Row rows[128]; DWORD count=0;
    static bool suffix(WCHAR *path, size_t capacity, const WCHAR *end) {
        size_t i=0;while(i<capacity && path[i])i++;
        while(*end && i+1<capacity)path[i++]=*end++;
        if(i==capacity||*end)return false;path[i]=0;return true;
    }
    void open() {
        WCHAR path[1024];UINT n=ARRAYSIZE(path);
        if(GetCurrentPackagePath(&n,path)!=ERROR_SUCCESS || !suffix(path,ARRAYSIZE(path),L"\\fhd-measure.txt"))return;
        HANDLE flag=CreateFile2(path,GENERIC_READ,FILE_SHARE_READ,OPEN_EXISTING,nullptr);
        if(flag==INVALID_HANDLE_VALUE)return;
        CloseHandle(flag);
        if(FAILED(FhdLocalPath(path,ARRAYSIZE(path))) || !suffix(path,ARRAYSIZE(path),L"\\fhd-present-times.bin"))return;
        file=CreateFile2(path,GENERIC_WRITE,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,CREATE_ALWAYS,nullptr);
        if(file==INVALID_HANDLE_VALUE)return;
        LARGE_INTEGER start;FILETIME utc;QueryPerformanceFrequency(&frequency);QueryPerformanceCounter(&start);GetSystemTimeAsFileTime(&utc);
        // Version 2 shared-texture records require a verified hardware adapter.
        Header h={0x31444846u,2u,(UINT64)frequency.QuadPart,((UINT64)utc.dwHighDateTime<<32)|utc.dwLowDateTime,(UINT64)start.QuadPart};
        DWORD written=0;
        if(!WriteFile(file,&h,sizeof(h),&written,nullptr)||written!=sizeof(h)){CloseHandle(file);file=INVALID_HANDLE_VALUE;}
    }
    void flush() {
        if(!count||file==INVALID_HANDLE_VALUE)return;
        DWORD bytes=count*sizeof(Row), n=0;
        if(!WriteFile(file,rows,bytes,&n,nullptr)||n!=bytes){CloseHandle(file);file=INVALID_HANDLE_VALUE;}
        count=0;
    }
    void record(UINT64 counter,UINT64 sequence,UINT presented,bool gpu) {
        if(file==INVALID_HANDLE_VALUE)return;
        rows[count++]={counter,sequence,presented,gpu?2u:1u};
        // Persist the first presentation even if the host exits before a batch fills.
        if(presented==1 || count==ARRAYSIZE(rows))flush();
    }
    bool enabled() const { return file != INVALID_HANDLE_VALUE; }
    void close(){flush();if(file!=INVALID_HANDLE_VALUE)CloseHandle(file);file=INVALID_HANDLE_VALUE;}
};
