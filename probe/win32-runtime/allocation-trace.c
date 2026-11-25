/* Bound diagnostics to large allocations and keep the native call intact. */
#define WIN32_LEAN_AND_MEAN
#include "loader-trace-output.h"

enum { sampleLimit = 2, frameLimit = 12, walkLimit = 20 };
static volatile LONG virtualSamples;
static volatile LONG heapSamples;

static char *Text(char *out, const char *text) {
    while (*text) *out++ = *text++;
    return out;
}

static char *Hex(char *out, ULONGLONG value, UINT digits) {
    static const char hex[] = "0123456789ABCDEF";
    while (digits) *out++ = hex[(value >> (4 * --digits)) & 15];
    return out;
}

static char *Prefix(char *out, const char *api, LONG sample) {
    out = Text(Text(Text(out, "asset alloc api="), api), " sample=");
    *out++ = (char)('0' + sample);
    return out;
}

static void WriteLine(char *line, char *out) {
    out = Text(out, "\r\n");
    AppendTrace(line, (DWORD)(out - line));
}

static ULONG_PTR ExecutableImageBase(ULONG_PTR pc) {
    MEMORY_BASIC_INFORMATION region;
    const DWORD executable = PAGE_EXECUTE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY;
    if (!pc || VirtualQuery((LPCVOID)pc, &region, sizeof(region)) != sizeof(region) ||
            region.Type != MEM_IMAGE || region.State != MEM_COMMIT ||
            !(region.Protect & executable) || (region.Protect & PAGE_GUARD)) return 0;
    return (ULONG_PTR)region.AllocationBase;
}

static void ObserveCaller(const char *api, LONG sample, ULONG_PTR caller) {
    char line[176];
    char *out = Text(Prefix(line, api, sample), " immediate_rva=");
    const ULONG_PTR callerImage = ExecutableImageBase(caller);
    if (!callerImage || caller < callerImage || caller - callerImage > MAXDWORD) {
        WriteLine(line, Text(out, "none"));
        return;
    }
    WriteLine(line, Hex(out, caller - callerImage, 8));

    ULONG_PTR low = 0, high = 0;
    GetCurrentThreadStackLimits(&low, &high);
    CONTEXT context;
    context.Rip = 0;
    context.Rsp = 0;
    RtlCaptureContext(&context);
    out = Text(Prefix(line, api, sample), " stack_rvas=");
    UINT recorded = 0;
    const char *stop = "limit";
    for (UINT walked = 0; walked < walkLimit && recorded < frameLimit; ++walked) {
        const DWORD64 stack = context.Rsp;
        if (!context.Rip || stack < low || stack >= high || high - stack < sizeof(ULONG_PTR) || (stack & 7u)) {
            stop = "bounds";
            break;
        }
        const ULONG_PTR executingImage = ExecutableImageBase(context.Rip);
        if (!executingImage) {
            stop = "image";
            break;
        }
        if (executingImage == callerImage && context.Rip >= callerImage && context.Rip - callerImage <= MAXDWORD) {
            if (recorded++) *out++ = ' ';
            out = Hex(out, context.Rip - callerImage, 8);
        }
        DWORD64 unwindImage = 0;
        PRUNTIME_FUNCTION entry = RtlLookupFunctionEntry(context.Rip, &unwindImage, NULL);
        if (!entry || unwindImage != executingImage) {
            stop = "metadata";
            break;
        }
        PVOID handlerData;
        DWORD64 establisherFrame;
        RtlVirtualUnwind(UNW_FLAG_NHANDLER, unwindImage, context.Rip, entry, &context,
            &handlerData, &establisherFrame, NULL);
        if (context.Rsp <= stack) {
            stop = "no-progress";
            break;
        }
    }
    if (!recorded) out = Text(out, "none");
    WriteLine(line, Text(Text(out, " stop="), stop));
}

static LONG BeginAllocation(const char *api, volatile LONG *samples, SIZE_T size, DWORD flags,
    LPVOID address, DWORD protection, ULONG_PTR caller) {
    if (size < ((SIZE_T)1 << 30) || InterlockedCompareExchange(samples, 0, 0) >= sampleLimit) return 0;
    const DWORD previous = GetLastError();
    LONG sample = 0;
    if (GetEnvironmentVariableW(L"XBOX_WIN32_LOADER_TRACE", NULL, 0) > 1) {
        sample = InterlockedIncrement(samples);
        if (sample > sampleLimit) sample = 0;
    }
    if (sample) {
        char line[176];
        char *out = Hex(Text(Prefix(line, api, sample), " begin size=0x"), size, 16);
        out = Hex(Text(out, " flags=0x"), flags, 8);
        if (samples == &virtualSamples) {
            out = Hex(Text(out, " protection=0x"), protection, 8);
            out = Text(out, address ? " requested_address=1" : " requested_address=0");
        }
        WriteLine(line, Hex(Text(out, " tid=0x"), GetCurrentThreadId(), 8));
        ObserveCaller(api, sample, caller);
    }
    SetLastError(previous);
    return sample;
}

static void EndAllocation(const char *api, LONG sample, BOOL succeeded) {
    const DWORD previous = GetLastError();
    char line[96];
    char *out = Text(Prefix(line, api, sample), succeeded ? " return=ok" : " return=null");
    WriteLine(line, Hex(Text(out, " last_error=0x"), previous, 8));
}

LPVOID WINAPI XboxTraceVirtualAlloc(LPVOID address, SIZE_T size, DWORD allocation, DWORD protection) {
    const LONG sample = BeginAllocation("VirtualAlloc", &virtualSamples, size, allocation, address,
        protection, (ULONG_PTR)__builtin_return_address(0));
    LPVOID result = VirtualAlloc(address, size, allocation, protection);
    if (sample) EndAllocation("VirtualAlloc", sample, result != NULL);
    return result;
}

LPVOID WINAPI XboxTraceHeapAlloc(HANDLE heap, DWORD flags, SIZE_T size) {
    const LONG sample = BeginAllocation("HeapAlloc", &heapSamples, size, flags, NULL, 0,
        (ULONG_PTR)__builtin_return_address(0));
    LPVOID result = HeapAlloc(heap, flags, size);
    if (sample) EndAllocation("HeapAlloc", sample, result != NULL);
    return result;
}
