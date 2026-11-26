/* Direct3D 12 residency switch and trace format, shared by the loader-trace
   adapter (d3d12-residency.c) and its host-side unit tests.

   Platform-neutral C (also valid C++) with no C runtime calls, so the
   CRT-free adapter and a hosted g++ test can both include it.

   Package-root file xbox-d3d12-residency.txt:
     absent or empty  default: native, counted when the pipeline hooks run
     native           count every call, forward every call
     pin              count; ID3D12Device::Evict returns S_OK without
                      reaching the driver, so nothing is ever evicted
     off              no residency hooks
   Trailing whitespace is ignored. Anything else is invalid and selects the
   default. */
#pragma once
#include <stdint.h>

#define XBOX_RESIDENCY_DEFAULT 0u
#define XBOX_RESIDENCY_NATIVE 1u
#define XBOX_RESIDENCY_PIN 2u
#define XBOX_RESIDENCY_OFF 3u
#define XBOX_RESIDENCY_INVALID 4u

/* ID3D12Device method-table slots: IUnknown (3) and ID3D12Object (4) come
   first, then ID3D12Device's 37 methods (7-43), ID3D12Device1 (44-46),
   ID3D12Device2 (47), ID3D12Device3 (48-50), ... ID3D12Device14 (82) and
   ID3D12Device15 (83-93). Counted from d3d12.idl in microsoft/DirectX-Headers. */
#define XBOX_RESIDENCY_SLOT_ALLOCATION_INFO 25u
#define XBOX_RESIDENCY_SLOT_MAKE_RESIDENT 34u
#define XBOX_RESIDENCY_SLOT_EVICT 35u
#define XBOX_RESIDENCY_SLOT_ENQUEUE_MAKE_RESIDENT 50u
#define XBOX_RESIDENCY_SLOT_REGISTER_TRIM 83u
/* IDXGIAdapter3: IUnknown (3), IDXGIObject (4), IDXGIAdapter (3), GetDesc1,
   GetDesc2, two content-protection methods, QueryVideoMemoryInfo (14),
   SetVideoMemoryReservation (15), then this. */
#define XBOX_RESIDENCY_SLOT_REGISTER_BUDGET_EVENT 16u

/* The loader-trace record filter (tools/xbox_diagnostics.py) keeps an
   "asset " line of at most 174 more characters. */
#define XBOX_RESIDENCY_LINE_MAX 180u

static inline uint32_t XboxResidencyParseMode(const char *text, uint32_t length) {
    while (length && (text[length - 1] == ' ' || text[length - 1] == '\t' || text[length - 1] == '\r' ||
                      text[length - 1] == '\n'))
        --length;
    if (!length) return XBOX_RESIDENCY_DEFAULT;
    static const char *const names[3] = {"native", "pin", "off"};
    static const uint32_t modes[3] = {XBOX_RESIDENCY_NATIVE, XBOX_RESIDENCY_PIN, XBOX_RESIDENCY_OFF};
    for (uint32_t name = 0; name < 3; ++name) {
        uint32_t at = 0;
        while (at < length && names[name][at] && text[at] == names[name][at]) ++at;
        if (at == length && !names[name][at]) return modes[name];
    }
    return XBOX_RESIDENCY_INVALID;
}

/* The behavior a parsed file selects: default and invalid both behave as
   native. */
static inline uint32_t XboxResidencyEffectiveMode(uint32_t parsed) {
    return parsed == XBOX_RESIDENCY_PIN || parsed == XBOX_RESIDENCY_OFF ? parsed : XBOX_RESIDENCY_NATIVE;
}

static inline const char *XboxResidencyModeName(uint32_t mode) {
    return mode == XBOX_RESIDENCY_PIN ? "pin" : mode == XBOX_RESIDENCY_OFF ? "off" : "native";
}

static inline const char *XboxResidencySourceName(uint32_t parsed, int fileFound) {
    if (!fileFound) return "default";
    return parsed == XBOX_RESIDENCY_INVALID ? "default override=invalid" :
           parsed == XBOX_RESIDENCY_DEFAULT ? "default override=empty" : "package-file";
}

/* Whether the hooks start: an explicit native or pin always starts them (the
   adapter then creates a device of its own once d3d12.dll is loaded); the
   default only when the pipeline hooks already create that device, so a
   default launch makes no D3D12 call it did not make before. */
static inline int XboxResidencyShouldStart(uint32_t parsed, int pipelineHooks) {
    if (parsed == XBOX_RESIDENCY_OFF) return 0;
    if (parsed == XBOX_RESIDENCY_NATIVE || parsed == XBOX_RESIDENCY_PIN) return 1;
    return pipelineHooks != 0;
}

/* Recently evicted objects, direct-mapped by address. A later MakeResident
   of the same address counts as a restore (an evict and re-residency round
   trip). Collisions and reused addresses make it approximate; it only feeds
   the trace. */
#define XBOX_RESIDENCY_EVICTED_SLOTS 16384u

static inline uint32_t XboxResidencyEvictedSlot(uint64_t address) {
    uint64_t mixed = (address >> 4) * 0x9E3779B97F4A7C15ull;
    return (uint32_t)(mixed >> 50) & (XBOX_RESIDENCY_EVICTED_SLOTS - 1u);
}

typedef struct XboxResidencyWindow {
    uint64_t evictCalls, makeResidentCalls, enqueueCalls;
    uint64_t evictObjects, evictBytes;
    uint64_t residentObjects, residentBytes; /* MakeResident and EnqueueMakeResident together */
    uint64_t restored;
    uint64_t trims, trimBytes;
    uint64_t failures;
} XboxResidencyWindow;

static inline int XboxResidencyWindowActive(const XboxResidencyWindow *window) {
    return window->evictCalls || window->makeResidentCalls || window->enqueueCalls || window->trims ||
        window->failures;
}

static inline uint64_t XboxResidencyKib(uint64_t bytes) {
    return bytes / 1024u + (bytes % 1024u ? 1u : 0u);
}

static inline char *XboxResidencyText(char *out, const char *end, const char *text) {
    while (*text && out < end) *out++ = *text++;
    return out;
}

static inline char *XboxResidencyNumber(char *out, const char *end, uint64_t value) {
    char text[20];
    int count = 0;
    do {
        text[count++] = (char)('0' + value % 10u);
        value /= 10u;
    } while (value && count < 20);
    while (count && out < end) *out++ = text[--count];
    return out;
}

/* Writes " label=value" whole, or nothing when it would pass end, so a line
   never carries a cut-off number. */
static inline char *XboxResidencyField(char *out, const char *end, const char *label, uint64_t value) {
    uint64_t width = 0;
    for (const char *at = label; *at; ++at) ++width;
    uint64_t rest = value;
    do {
        ++width;
        rest /= 10u;
    } while (rest);
    if (width > (uint64_t)(end - out)) return out;
    return XboxResidencyNumber(XboxResidencyText(out, end, label), end, value);
}

/* "asset residency evict=... utc=" for one second of calls; the caller
   appends the UTC stamp (16 characters) and the line end. At most
   XBOX_RESIDENCY_LINE_MAX - 16 characters are written, and " utc=" always
   fits: a field that would not is left out. */
static inline char *XboxResidencyFormatWindow(char *out, const XboxResidencyWindow *window) {
    const char *end = out + XBOX_RESIDENCY_LINE_MAX - 16u;
    const char *fields = end - 5;
    out = XboxResidencyText(out, fields, "asset residency");
    out = XboxResidencyField(out, fields, " evict=", window->evictCalls);
    out = XboxResidencyField(out, fields, " makeresident=", window->makeResidentCalls);
    out = XboxResidencyField(out, fields, " enqueue=", window->enqueueCalls);
    out = XboxResidencyField(out, fields, " evict_obj=", window->evictObjects);
    out = XboxResidencyField(out, fields, " evict_kib=", XboxResidencyKib(window->evictBytes));
    out = XboxResidencyField(out, fields, " res_obj=", window->residentObjects);
    out = XboxResidencyField(out, fields, " res_kib=", XboxResidencyKib(window->residentBytes));
    out = XboxResidencyField(out, fields, " restored=", window->restored);
    if (window->trims) out = XboxResidencyField(out, fields, " trim=", window->trims);
    if (window->failures) out = XboxResidencyField(out, fields, " failed=", window->failures);
    return XboxResidencyText(out, end, " utc=");
}

/* "asset residency-total mode=... t_s=... ..." every 60 s with the process
   totals, whether or not anything happened, so a quiet trace still shows the
   hooks alive. */
static inline char *XboxResidencyFormatTotal(char *out, const XboxResidencyWindow *total, uint32_t mode,
        uint64_t seconds) {
    const char *end = out + XBOX_RESIDENCY_LINE_MAX - 16u;
    const char *fields = end - 5;
    out = XboxResidencyText(XboxResidencyText(out, fields, "asset residency-total mode="), fields,
        XboxResidencyModeName(mode));
    out = XboxResidencyField(out, fields, " t_s=", seconds);
    out = XboxResidencyField(out, fields, " evict_obj=", total->evictObjects);
    out = XboxResidencyField(out, fields, " evict_mib=", XboxResidencyKib(total->evictBytes) / 1024u);
    out = XboxResidencyField(out, fields, " res_obj=", total->residentObjects);
    out = XboxResidencyField(out, fields, " res_mib=", XboxResidencyKib(total->residentBytes) / 1024u);
    out = XboxResidencyField(out, fields, " restored=", total->restored);
    out = XboxResidencyField(out, fields, " trim=", total->trims);
    out = XboxResidencyField(out, fields, " trim_mib=", XboxResidencyKib(total->trimBytes) / 1024u);
    out = XboxResidencyField(out, fields, " failed=", total->failures);
    return XboxResidencyText(out, end, " utc=");
}
