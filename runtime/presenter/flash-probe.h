#pragma once
// Opt-in white-flash probe for shared-texture frames (clankur/GabeBox#89). Enabled by
// fhd-flash-probe.txt in the package root; without it HostedGpuReceiver never creates one.
//
// For each drawn frame the presenter copies SDL_GPU_PROBE_ROWS full-width rows of the exchange
// texture (after its GPU wait on the ready fence) and of its own output target (after the draw)
// into small staging textures. It maps them a few frames later with D3D11_MAP_FLAG_DO_NOT_WAIT,
// so the CPU never waits on the GPU; a frame whose readback ring is still busy is skipped and
// counted. The producer samples the same rows of the game's renderbuffer before its copy and
// publishes them through the ".probe" sidecar (sdl-gpu-probe.h). Equal hashes for one sequence
// mean the exchange delivered the game's pixels unchanged.
//
// LocalState outputs:
//   fhd-flash-probe.bin      48-byte header, then one 32-byte record per sampled frame and source
//   fhd-flash-summary.txt    the counters below, rewritten at most once a second
//   fhd-flash-<utc>-<event>-<role>-<sequence>.bmp  480x270 copies of the exchange texture: one
//                            baseline frame, then the first frame of up to three white runs and the
//                            frame drawn before each
#include <windows.h>
#include <d3d11_4.h>
#include <wrl/client.h>
#include <initializer_list>
#include <new>
#include "frame-trace.h"
#include "flash-detector.h"
#include "gpu-receiver.h"
#include "../shared/sdl-gpu-probe.h"

class FhdFlashProbe {
public:
    static constexpr UINT ThumbWidth = 480, ThumbHeight = 270;
    enum Kind : UINT8 { Game = 1, Exchange = 2, Output = 3, Capture = 4 };
    enum Role : UINT16 { RoleFlash = 1, RoleBefore = 2, RoleBaseline = 3 };
    struct Header { UINT32 magic, version; UINT64 frequency, utcFileTime, counter; UINT32 rows, cells, white, reserved; };
    struct Record {
        UINT64 counter, sequence;
        UINT8 kind, slot; UINT16 flags;
        UINT16 luma, white, cells; UINT8 min, max; UINT32 hash;
    };
    static_assert(sizeof(Header) == 48 && sizeof(Record) == 32, "flash probe file ABI");

    // Null when the package has no marker or the probe cannot start; the receiver then runs as before.
    static FhdFlashProbe *create(const WCHAR *mailboxPath) {
        if (!marker()) return nullptr;
        void *memory = HeapAlloc(GetProcessHeap(), 0, sizeof(FhdFlashProbe));
        if (!memory) return nullptr;
        auto *probe = new (memory) FhdFlashProbe;
        if (!probe->open(mailboxPath)) { destroy(probe); return nullptr; }
        return probe;
    }
    static void destroy(FhdFlashProbe *probe) {
        if (!probe) return;
        probe->close();
        probe->~FhdFlashProbe();
        HeapFree(GetProcessHeap(), 0, probe);
    }

    const HostedFlashCounters &counters() const { return counters_; }

    // Drops device objects and pending readbacks when the receiver disconnects. Frames still in
    // the detectors are written with the frames that followed them.
    void reset() {
        Item item;
        while (exchange_.detector.drain(item)) emit(nullptr, item);
        while (output_.detector.drain(item)) emit(nullptr, item);
        exchange_.clear(); output_.clear();
        for (auto &thumb : thumbs_) thumb = Thumb{};
        for (auto &shot : shots_) shot = Shot{};
        thumbNext_ = 0;
        writeRecords();
    }

    // Call after the shared texture's draw and before its done fence is signalled.
    void sample(ID3D11Device *device, ID3D11DeviceContext *context, ID3D11Texture2D *source, UINT slot,
                UINT64 sequence, ID3D11RenderTargetView *output) {
        LARGE_INTEGER now{};
        QueryPerformanceCounter(&now);
        queue(exchange_, device, context, source, slot, sequence, now.QuadPart);
        Microsoft::WRL::ComPtr<ID3D11Resource> resource;
        Microsoft::WRL::ComPtr<ID3D11Texture2D> target;
        if (output) output->GetResource(&resource);
        if (resource && SUCCEEDED(resource.As(&target))) queue(output_, device, context, target.Get(), slot, sequence, now.QuadPart);
    }

    // A render target to receive this frame's thumbnail, or null once no capture remains.
    ID3D11RenderTargetView *thumbnail(ID3D11Device *device, UINT64 sequence) {
        if (thumbFailed_ || (flashEvents_ >= MaxFlashEvents && baselineDone_)) return nullptr;
        auto &thumb = thumbs_[thumbNext_];
        if (!thumb.view) {
            D3D11_TEXTURE2D_DESC desc{};
            desc.Width = ThumbWidth; desc.Height = ThumbHeight; desc.MipLevels = desc.ArraySize = 1;
            desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM; desc.SampleDesc.Count = 1;
            desc.Usage = D3D11_USAGE_DEFAULT; desc.BindFlags = D3D11_BIND_RENDER_TARGET;
            if (FAILED(device->CreateTexture2D(&desc, nullptr, &thumb.texture)) ||
                    FAILED(device->CreateRenderTargetView(thumb.texture.Get(), nullptr, &thumb.view))) {
                thumb = Thumb{}; thumbFailed_ = true; return nullptr;
            }
        }
        thumb.sequence = sequence;
        thumbNext_ = (thumbNext_ + 1) % Thumbs;
        return thumb.view.Get();
    }

    // Collects finished readbacks and producer records without waiting. Call once per refresh.
    void poll(ID3D11DeviceContext *context) {
        collect(exchange_, context);
        collect(output_, context);
        readProducer(context);
        collectShots(context);
        const DWORD now = GetTickCount();
        if (count_ && now - flushedAt_ >= 2000) writeRecords();
        if (now - summaryAt_ >= 1000) writeSummary(now);
    }

private:
    static constexpr UINT Depth = SDL_GPU_PROBE_DEPTH, Thumbs = 12, MaxFlashEvents = 3, BaselineFrame = 600;
    static constexpr UINT Pairs = 64;
    struct Item { SdlGpuProbeStats stats{}; UINT16 flags = 0; UINT8 kind = 0, slot = 0; UINT64 counter = 0, sequence = 0; };
    struct Readback {
        Microsoft::WRL::ComPtr<ID3D11Texture2D> texture;
        UINT64 sequence = 0; INT64 counter = 0; UINT slot = 0, width = 0;
        DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN; bool pending = false;
    };
    struct Lane {
        explicit Lane(Kind source) : kind(source) {}
        Kind kind;
        Readback entries[Depth];
        UINT next = 0;
        FhdFlash::Detector<Item> detector;
        void clear() { for (auto &entry : entries) entry = Readback{}; next = 0; detector.reset(); }
    };
    struct Thumb { Microsoft::WRL::ComPtr<ID3D11Texture2D> texture; Microsoft::WRL::ComPtr<ID3D11RenderTargetView> view; UINT64 sequence = 0; };
    struct Shot { Microsoft::WRL::ComPtr<ID3D11Texture2D> staging; Item item; UINT event = 0; UINT16 role = 0; bool pending = false; };
    struct Pair { UINT64 sequence = 0; UINT32 hash = 0; };

    Lane exchange_{Exchange}, output_{Output};
    FhdFlash::Detector<Item> game_;
    Thumb thumbs_[Thumbs];
    Shot shots_[2];
    Pair gameHashes_[Pairs], exchangeHashes_[Pairs];
    HostedFlashCounters counters_{};
    SdlGpuProbeMailbox *box_ = nullptr;
    HANDLE boxFile_ = INVALID_HANDLE_VALUE, file_ = INVALID_HANDLE_VALUE, summary_ = INVALID_HANDLE_VALUE;
    Header header_{};
    Record records_[128];
    UINT count_ = 0, thumbNext_ = 0, flashEvents_ = 0;
    INT64 read_ = 0;
    DWORD flushedAt_ = 0, summaryAt_ = 0;
    UINT64 summarized_[17]{};
    bool thumbFailed_ = false, baselineDone_ = false, lastWhite_[5]{};
    WCHAR local_[1024]{}, stamp_[20]{};

    static bool marker() {
        WCHAR path[1024]; UINT32 n = ARRAYSIZE(path);
        if (GetCurrentPackagePath(&n, path) != ERROR_SUCCESS || !FhdFrameTrace::suffix(path, ARRAYSIZE(path), L"\\fhd-flash-probe.txt"))
            return false;
        HANDLE flag = CreateFile2(path, GENERIC_READ, FILE_SHARE_READ, OPEN_EXISTING, nullptr);
        if (flag == INVALID_HANDLE_VALUE) return false;
        CloseHandle(flag);
        return true;
    }
    static void append(WCHAR *path, size_t capacity, UINT64 value) {
        WCHAR digits[24]; UINT n = 0;
        do { digits[n++] = static_cast<WCHAR>(L'0' + value % 10); value /= 10; } while (value);
        WCHAR text[24]; for (UINT i = 0; i < n; ++i) text[i] = digits[n - 1 - i]; text[n] = 0;
        FhdFrameTrace::suffix(path, capacity, text);
    }
    HANDLE createLocal(const WCHAR *name) const {
        WCHAR path[1024];
        for (UINT i = 0; i < ARRAYSIZE(path); ++i) { path[i] = local_[i]; if (!path[i]) break; }
        if (!FhdFrameTrace::suffix(path, ARRAYSIZE(path), name)) return INVALID_HANDLE_VALUE;
        return CreateFile2(path, GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, CREATE_ALWAYS, nullptr);
    }

    bool open(const WCHAR *mailboxPath) {
        if (FAILED(FhdLocalPath(local_, ARRAYSIZE(local_)))) return false;
        file_ = createLocal(L"\\fhd-flash-probe.bin");
        if (file_ == INVALID_HANDLE_VALUE) return false;
        LARGE_INTEGER frequency{}, start{};
        FILETIME utc{};
        QueryPerformanceFrequency(&frequency); QueryPerformanceCounter(&start); GetSystemTimeAsFileTime(&utc);
        header_ = {0x46444846u, 1u, static_cast<UINT64>(frequency.QuadPart),
                   (static_cast<UINT64>(utc.dwHighDateTime) << 32) | utc.dwLowDateTime, static_cast<UINT64>(start.QuadPart),
                   SDL_GPU_PROBE_ROWS, SDL_GPU_PROBE_CELLS, SDL_GPU_PROBE_WHITE, 0};
        DWORD written = 0;
        if (!WriteFile(file_, &header_, sizeof(header_), &written, nullptr) || written != sizeof(header_)) return false;
        SYSTEMTIME time{};
        if (FileTimeToSystemTime(&utc, &time)) {
            const UINT values[] = {time.wYear, time.wMonth, time.wDay, time.wHour, time.wMinute, time.wSecond};
            const UINT widths[] = {4, 2, 2, 2, 2, 2};
            UINT n = 0;
            for (UINT i = 0; i < 6; ++i) {
                if (i == 3) stamp_[n++] = L'T';
                for (UINT d = widths[i], v = values[i]; d; --d) { stamp_[n + d - 1] = static_cast<WCHAR>(L'0' + v % 10); v /= 10; }
                n += widths[i];
            }
            stamp_[n] = 0;
        }
        summary_ = createLocal(L"\\fhd-flash-summary.txt");
        counters_.enabled = 1;
        openSidecar(mailboxPath);
        flushedAt_ = summaryAt_ = GetTickCount() - 1000;
        return true;
    }
    // The producer half is optional: a failure here leaves presenter-side sampling running.
    void openSidecar(const WCHAR *mailboxPath) {
        WCHAR path[1024]; UINT n = 0;
        while (mailboxPath[n] && n < ARRAYSIZE(path) - 8) { path[n] = mailboxPath[n]; ++n; }
        if (mailboxPath[n]) return;
        path[n] = 0;
        if (!FhdFrameTrace::suffix(path, ARRAYSIZE(path), L".probe")) return;
        CREATEFILE2_EXTENDED_PARAMETERS flags{}; flags.dwSize = sizeof(flags);
        flags.dwFileAttributes = FILE_ATTRIBUTE_TEMPORARY; flags.dwFileFlags = FILE_FLAG_DELETE_ON_CLOSE;
        boxFile_ = CreateFile2(path, GENERIC_READ | GENERIC_WRITE | DELETE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                               CREATE_NEW, &flags);
        bool created = true;
        if (boxFile_ == INVALID_HANDLE_VALUE && GetLastError() == ERROR_FILE_EXISTS) {
            created = false;
            boxFile_ = CreateFile2(path, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, OPEN_EXISTING, nullptr);
        }
        if (boxFile_ == INVALID_HANDLE_VALUE) return;
        LARGE_INTEGER size{};
        if (!created && (!GetFileSizeEx(boxFile_, &size) || size.QuadPart != sizeof(SdlGpuProbeMailbox))) return;
        HANDLE mapping = CreateFileMappingFromApp(boxFile_, nullptr, PAGE_READWRITE, sizeof(SdlGpuProbeMailbox), nullptr);
        if (!mapping) return;
        box_ = static_cast<SdlGpuProbeMailbox *>(MapViewOfFileFromApp(mapping, FILE_MAP_WRITE, 0, sizeof(SdlGpuProbeMailbox)));
        CloseHandle(mapping);
        if (!box_) return;
        read_ = InterlockedCompareExchange64(&box_->written, 0, 0);
        InterlockedExchange(&box_->magic, SDL_GPU_PROBE_MAGIC);
    }
    void close() {
        Item item;
        while (game_.drain(item)) emit(nullptr, item);
        reset();
        writeSummary(GetTickCount(), true);
        if (box_) { InterlockedExchange(&box_->magic, 0); UnmapViewOfFile(box_); box_ = nullptr; }
        for (HANDLE *handle : {&boxFile_, &file_, &summary_})
            if (*handle != INVALID_HANDLE_VALUE) { CloseHandle(*handle); *handle = INVALID_HANDLE_VALUE; }
    }

    static bool bgra(DXGI_FORMAT format) {
        return format == DXGI_FORMAT_B8G8R8A8_UNORM || format == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB ||
               format == DXGI_FORMAT_B8G8R8A8_TYPELESS;
    }
    static bool rgba(DXGI_FORMAT format) {
        return format == DXGI_FORMAT_R8G8B8A8_UNORM || format == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB ||
               format == DXGI_FORMAT_R8G8B8A8_TYPELESS;
    }
    void queue(Lane &lane, ID3D11Device *device, ID3D11DeviceContext *context, ID3D11Texture2D *source,
               UINT slot, UINT64 sequence, INT64 counter) {
        D3D11_TEXTURE2D_DESC desc{};
        source->GetDesc(&desc);
        if (desc.SampleDesc.Count != 1 || !desc.Width || !desc.Height || (!bgra(desc.Format) && !rgba(desc.Format))) return;
        auto &entry = lane.entries[lane.next];
        if (entry.pending) collect(lane, context);
        if (entry.pending) { ++counters_.skipped; return; }
        if (!entry.texture || entry.width != desc.Width || entry.format != desc.Format) {
            entry.texture.Reset();
            D3D11_TEXTURE2D_DESC rows{};
            rows.Width = desc.Width; rows.Height = SDL_GPU_PROBE_ROWS; rows.MipLevels = rows.ArraySize = 1;
            rows.Format = desc.Format; rows.SampleDesc.Count = 1;
            rows.Usage = D3D11_USAGE_STAGING; rows.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
            if (FAILED(device->CreateTexture2D(&rows, nullptr, &entry.texture))) { entry.texture.Reset(); ++counters_.skipped; return; }
            entry.width = desc.Width; entry.format = desc.Format;
        }
        for (UINT row = 0; row < SDL_GPU_PROBE_ROWS; ++row) {
            const UINT y = SdlGpuProbeRowY(row, desc.Height);
            const D3D11_BOX box{0, y, 0, desc.Width, y + 1, 1};
            context->CopySubresourceRegion(entry.texture.Get(), 0, 0, row, 0, source, 0, &box);
        }
        entry.sequence = sequence; entry.counter = counter; entry.slot = slot; entry.pending = true;
        lane.next = (lane.next + 1) % Depth;
    }
    void collect(Lane &lane, ID3D11DeviceContext *context) {
        for (UINT k = 0; k < Depth; ++k) {
            auto &entry = lane.entries[(lane.next + k) % Depth];
            if (!entry.pending) continue;
            D3D11_MAPPED_SUBRESOURCE mapped{};
            const HRESULT hr = context->Map(entry.texture.Get(), 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &mapped);
            if (hr == DXGI_ERROR_WAS_STILL_DRAWING) break;
            entry.pending = false;
            if (FAILED(hr)) { ++counters_.skipped; continue; }
            Item item;
            SdlGpuProbeMeasure(static_cast<const uint8_t *>(mapped.pData), mapped.RowPitch, entry.width, bgra(entry.format), &item.stats);
            context->Unmap(entry.texture.Get(), 0);
            item.kind = lane.kind; item.slot = static_cast<UINT8>(entry.slot);
            item.counter = static_cast<UINT64>(entry.counter); item.sequence = entry.sequence;
            Item out;
            if (lane.detector.push(item, out)) emit(context, out);
        }
    }
    void readProducer(ID3D11DeviceContext *context) {
        if (!box_) return;
        const LONG state = InterlockedCompareExchange(&box_->producer, 0, 0);
        counters_.producer = static_cast<UINT32>(state);
        counters_.gameSkipped = static_cast<UINT64>(InterlockedCompareExchange64(&box_->skipped, 0, 0));
        const INT64 written = InterlockedCompareExchange64(&box_->written, 0, 0);
        if (written < read_) read_ = 0; // a new producer started over
        if (written - read_ > SDL_GPU_PROBE_RING) {
            counters_.gameLost += static_cast<UINT64>(written - read_ - SDL_GPU_PROBE_RING);
            read_ = written - SDL_GPU_PROBE_RING;
        }
        for (; read_ < written; ++read_) {
            auto &record = box_->ring[read_ % SDL_GPU_PROBE_RING];
            const INT64 first = InterlockedCompareExchange64(&record.sequence, 0, 0);
            Item item;
            item.stats = record.stats; item.counter = static_cast<UINT64>(record.counter);
            item.slot = static_cast<UINT8>(record.slot); item.kind = Game;
            const INT64 second = InterlockedCompareExchange64(&record.sequence, 0, 0);
            const INT64 now = InterlockedCompareExchange64(&box_->written, 0, 0);
            if (first <= 0 || first != second || now - read_ > SDL_GPU_PROBE_RING) { ++counters_.gameLost; continue; }
            item.sequence = static_cast<UINT64>(first);
            Item out;
            if (game_.push(item, out)) emit(context, out);
        }
    }

    static Pair *find(Pair (&pairs)[Pairs], UINT64 sequence) {
        auto &pair = pairs[sequence % Pairs];
        return pair.sequence == sequence ? &pair : nullptr;
    }
    // Writes one classified record, compares the game and exchange hashes for its sequence, and
    // starts a capture at the first frame of a white run.
    void emit(ID3D11DeviceContext *context, Item item) {
        if (item.kind == Game || item.kind == Exchange) {
            auto &own = item.kind == Game ? gameHashes_ : exchangeHashes_;
            auto &other = item.kind == Game ? exchangeHashes_ : gameHashes_;
            if (const Pair *match = find(other, item.sequence)) {
                if (match->hash == item.stats.hash) ++counters_.matched;
                else { ++counters_.mismatched; item.flags |= FhdFlash::Mismatch; }
            }
            own[item.sequence % Pairs] = {item.sequence, item.stats.hash};
        }
        const bool white = FhdFlash::white(item.flags);
        switch (item.kind) {
        case Game: ++counters_.gameFrames; if (white) ++counters_.whiteGame; break;
        case Exchange:
            ++counters_.frames;
            if (white) { ++counters_.white; if (!lastWhite_[Exchange]) ++counters_.runs; }
            break;
        case Output: ++counters_.outputFrames; if (white) ++counters_.whiteOutput; break;
        default: break;
        }
        if (white && !counters_.firstUtc) {
            const INT64 delta = static_cast<INT64>(item.counter - header_.counter);
            counters_.firstUtc = header_.utcFileTime + static_cast<UINT64>(delta * 10000000 / static_cast<INT64>(header_.frequency));
        }
        record(item);
        const bool runStart = white && !lastWhite_[item.kind];
        lastWhite_[item.kind] = white;
        if (!context) return;
        if (runStart && flashEvents_ < MaxFlashEvents) capture(context, item, RoleFlash);
        if (item.kind == Exchange && !baselineDone_ && counters_.frames >= BaselineFrame) capture(context, item, RoleBaseline);
    }
    void record(const Item &item) {
        if (file_ == INVALID_HANDLE_VALUE) return;
        records_[count_++] = {item.counter, item.sequence, item.kind, item.slot, item.flags,
                              item.stats.luma, item.stats.white, item.stats.cells, item.stats.min, item.stats.max, item.stats.hash};
        if (count_ == ARRAYSIZE(records_) || (item.kind != Game && FhdFlash::white(item.flags))) writeRecords();
    }
    void writeRecords() {
        flushedAt_ = GetTickCount();
        if (!count_ || file_ == INVALID_HANDLE_VALUE) { count_ = 0; return; }
        const DWORD bytes = count_ * sizeof(Record);
        DWORD written = 0;
        if (!WriteFile(file_, records_, bytes, &written, nullptr) || written != bytes) { CloseHandle(file_); file_ = INVALID_HANDLE_VALUE; }
        count_ = 0;
    }

    void capture(ID3D11DeviceContext *context, const Item &item, UINT16 role) {
        if (shots_[0].pending || shots_[1].pending) return;
        int found = -1, before = -1;
        for (UINT i = 0; i < Thumbs; ++i) {
            const UINT64 tag = thumbs_[i].sequence;
            if (!thumbs_[i].texture || !tag) continue;
            if (tag == item.sequence) found = static_cast<int>(i);
            else if (tag < item.sequence && (before < 0 || tag > thumbs_[before].sequence)) before = static_cast<int>(i);
        }
        if (found < 0) return;
        Microsoft::WRL::ComPtr<ID3D11Device> device;
        context->GetDevice(&device);
        const UINT event = role == RoleBaseline ? 0 : flashEvents_ + 1;
        if (!stage(device.Get(), context, shots_[0], found, item, event, role)) return;
        if (role == RoleBaseline) baselineDone_ = true;
        else {
            ++flashEvents_;
            if (before >= 0) {
                Item previous = item;
                previous.sequence = thumbs_[before].sequence;
                previous.flags = 0;
                stage(device.Get(), context, shots_[1], before, previous, event, RoleBefore);
            }
        }
    }
    bool stage(ID3D11Device *device, ID3D11DeviceContext *context, Shot &shot, int thumb, const Item &item, UINT event, UINT16 role) {
        if (!shot.staging) {
            D3D11_TEXTURE2D_DESC desc{};
            desc.Width = ThumbWidth; desc.Height = ThumbHeight; desc.MipLevels = desc.ArraySize = 1;
            desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM; desc.SampleDesc.Count = 1;
            desc.Usage = D3D11_USAGE_STAGING; desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
            if (!device || FAILED(device->CreateTexture2D(&desc, nullptr, &shot.staging))) { shot.staging.Reset(); return false; }
        }
        context->CopyResource(shot.staging.Get(), thumbs_[thumb].texture.Get());
        shot.item = item; shot.event = event; shot.role = role; shot.pending = true;
        return true;
    }
    void collectShots(ID3D11DeviceContext *context) {
        for (auto &shot : shots_) {
            if (!shot.pending) continue;
            D3D11_MAPPED_SUBRESOURCE mapped{};
            const HRESULT hr = context->Map(shot.staging.Get(), 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &mapped);
            if (hr == DXGI_ERROR_WAS_STILL_DRAWING) continue;
            shot.pending = false;
            if (FAILED(hr)) continue;
            const bool saved = writeBitmap(shot, static_cast<const BYTE *>(mapped.pData), mapped.RowPitch);
            context->Unmap(shot.staging.Get(), 0);
            if (!saved) continue;
            ++counters_.captures;
            Item event = shot.item;
            event.kind = Capture; event.slot = static_cast<UINT8>(shot.event); event.flags = shot.role;
            LARGE_INTEGER now{};
            QueryPerformanceCounter(&now);
            event.counter = static_cast<UINT64>(now.QuadPart);
            event.stats.hash = 0;
            record(event);
            writeRecords();
        }
    }
    // A 24-bit bottom-up BMP, the most widely readable form.
    bool writeBitmap(const Shot &shot, const BYTE *pixels, UINT pitch) {
        constexpr UINT stride = ThumbWidth * 3, image = stride * ThumbHeight, size = 54 + image;
        static_assert(stride % 4 == 0, "BMP rows need no padding");
        BYTE *bytes = static_cast<BYTE *>(HeapAlloc(GetProcessHeap(), 0, size));
        if (!bytes) return false;
        auto put = [&](UINT offset, UINT value, UINT width) { for (UINT i = 0; i < width; ++i) bytes[offset + i] = static_cast<BYTE>(value >> (8 * i)); };
        for (UINT i = 0; i < 54; ++i) bytes[i] = 0;
        bytes[0] = 'B'; bytes[1] = 'M'; put(2, size, 4); put(10, 54, 4);
        put(14, 40, 4); put(18, ThumbWidth, 4); put(22, ThumbHeight, 4); put(26, 1, 2); put(28, 24, 2); put(34, image, 4);
        for (UINT y = 0; y < ThumbHeight; ++y) {
            const BYTE *source = pixels + static_cast<size_t>(y) * pitch;
            BYTE *row = bytes + 54 + static_cast<size_t>(ThumbHeight - 1 - y) * stride;
            for (UINT x = 0; x < ThumbWidth; ++x) { row[3 * x] = source[4 * x + 2]; row[3 * x + 1] = source[4 * x + 1]; row[3 * x + 2] = source[4 * x]; }
        }
        static constexpr const WCHAR *roles[] = {L"", L"-flash-", L"-before-", L"-baseline-"};
        WCHAR name[96] = L"\\fhd-flash-";
        FhdFrameTrace::suffix(name, ARRAYSIZE(name), stamp_);
        FhdFrameTrace::suffix(name, ARRAYSIZE(name), L"-");
        append(name, ARRAYSIZE(name), shot.event);
        FhdFrameTrace::suffix(name, ARRAYSIZE(name), roles[shot.role < 4 ? shot.role : 0]);
        append(name, ARRAYSIZE(name), shot.item.sequence);
        FhdFrameTrace::suffix(name, ARRAYSIZE(name), L".bmp");
        HANDLE file = createLocal(name);
        DWORD written = 0;
        const bool saved = file != INVALID_HANDLE_VALUE && WriteFile(file, bytes, size, &written, nullptr) && written == size;
        if (file != INVALID_HANDLE_VALUE) CloseHandle(file);
        HeapFree(GetProcessHeap(), 0, bytes);
        return saved;
    }

    void writeSummary(DWORD now, bool force = false) {
        if (summary_ == INVALID_HANDLE_VALUE) return;
        const auto &c = counters_;
        const UINT64 values[17] = {c.enabled, c.producer, c.frames, c.outputFrames, c.gameFrames, c.white, c.runs,
            c.whiteOutput, c.whiteGame, c.matched, c.mismatched, c.skipped, c.gameSkipped, c.gameLost, c.captures,
            c.firstUtc, header_.utcFileTime};
        summaryAt_ = now;
        bool changed = force;
        for (UINT i = 0; i < 17; ++i) changed |= values[i] != summarized_[i];
        if (!changed) return;
        static constexpr const char *names[16] = {"flash_probe", "flash_producer", "flash_frames", "flash_output_frames",
            "flash_game_frames", "white_frames", "white_runs", "white_output_frames", "white_game_frames", "flash_matched",
            "flash_mismatched", "flash_skipped", "flash_game_skipped", "flash_game_lost", "flash_captures", "white_first_filetime"};
        char text[1024]; UINT n = 0;
        auto add = [&](const char *s) { while (*s && n + 1 < sizeof(text)) text[n++] = *s++; };
        auto number = [&](UINT64 v) { char d[24]; UINT k = 0; do { d[k++] = static_cast<char>('0' + v % 10); v /= 10; } while (v); while (k && n + 1 < sizeof(text)) text[n++] = d[--k]; };
        for (UINT i = 0; i < 16; ++i) { add(names[i]); add("="); number(values[i]); add("\r\n"); }
        add("flash_start_filetime="); number(header_.utcFileTime); add("\r\n");
        LARGE_INTEGER start{};
        DWORD written = 0;
        if (!SetFilePointerEx(summary_, start, nullptr, FILE_BEGIN) || !WriteFile(summary_, text, n, &written, nullptr) ||
                written != n || !SetEndOfFile(summary_)) {
            CloseHandle(summary_); summary_ = INVALID_HANDLE_VALUE; return;
        }
        for (UINT i = 0; i < 17; ++i) summarized_[i] = values[i];
    }
};
