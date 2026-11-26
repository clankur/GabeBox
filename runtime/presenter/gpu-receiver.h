#pragma once
#include <windows.h>
#include <d3d11_4.h>
#include "../shared/sdl-gpu-protocol.h"

// How the shared-texture exchange behaved since Open, for the session and panel reports.
// notReady: polls that found a published frame whose producer copy had not yet completed;
//   the frame was left for a later refresh, never drawn (the ready-fence gate at work).
// held: polls that kept a completed frame, newer than the one drawn, for the next refresh.
// released: frames acknowledged on the done fence without being drawn (superseded).
// drawn: frames drawn after a GPU wait on their ready fence.
// readyValue/doneValue: the drawn slot's completed producer fence when it was polled,
//   and the consumer fence value signalled for it (the drawn sequence).
// The opt-in white-flash probe (flash-probe.h), all zero unless fhd-flash-probe.txt is in the
// package root. frames/outputFrames/gameFrames count sampled frames of the exchange texture, the
// presenter's output and the game's renderbuffer (from the producer). white/whiteOutput/whiteGame
// count those flagged as a flash or uniformly white, runs counts white runs of exchange frames.
// matched/mismatched compare the game and exchange hashes of one sequence. producer is 0 before
// a producer reads the probe sidecar, 1 while it samples, else its failed setup HRESULT.
// firstUtc is the FILETIME of the first white frame from any source.
struct HostedFlashCounters {
    UINT64 enabled, producer, frames, outputFrames, gameFrames, white, runs, whiteOutput, whiteGame;
    UINT64 matched, mismatched, skipped, gameSkipped, gameLost, captures, firstUtc;
};
struct HostedGpuCounters { UINT64 notReady, held, released, drawn, readyValue, doneValue; HostedFlashCounters flash; };

// ISO 8601 UTC with milliseconds, or an empty string for 0.
inline void FhdUtcText(UINT64 filetime, WCHAR (&out)[32]) {
    out[0] = 0;
    FILETIME value{static_cast<DWORD>(filetime), static_cast<DWORD>(filetime >> 32)};
    SYSTEMTIME t{};
    if (!filetime || !FileTimeToSystemTime(&value, &t)) return;
    const UINT fields[] = {t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond, t.wMilliseconds};
    const UINT widths[] = {4, 2, 2, 2, 2, 2, 3};
    const WCHAR separators[] = {L'-', L'-', L'T', L':', L':', L'.', L'Z'};
    UINT n = 0;
    for (UINT i = 0; i < 7; ++i) {
        for (UINT d = widths[i], v = fields[i]; d; --d) { out[n + d - 1] = static_cast<WCHAR>(L'0' + v % 10); v /= 10; }
        n += widths[i];
        out[n++] = separators[i];
    }
    out[n] = 0;
}

class FhdFlashProbe;

// Use on the thread owning the dedicated presentation context; drawing replaces its pipeline state.
class HostedGpuReceiver {
public:
    HostedGpuReceiver() = default;
    ~HostedGpuReceiver();
    HostedGpuReceiver(const HostedGpuReceiver&) = delete;
    HostedGpuReceiver& operator=(const HostedGpuReceiver&) = delete;
    // Create the sidecar before launching SDL; path excludes the ".gpu" suffix.
    HRESULT Open(const wchar_t *mailboxPath);
    // S_OK drew a unique sequence. S_FALSE has no new frame. A failure leaves CPU delivery enabled.
    HRESULT Draw(ID3D11Device *device, ID3D11DeviceContext *context, ID3D11RenderTargetView *target,
                 UINT width, UINT height, UINT64 &sequence, LONG hostPointer = -1);
    HRESULT DrawCpu(ID3D11Device *device, ID3D11DeviceContext *context, ID3D11RenderTargetView *target,
                    UINT width, UINT height, const BYTE *pixels, UINT sourceWidth, UINT sourceHeight, LONG hostPointer = -1);
    UINT SourceWidth() const { return lastWidth_; }
    UINT SourceHeight() const { return lastHeight_; }
    const char *Stage() const { return stage_; }
    bool Active() const;
    UINT64 ProducerSequence() const;
    HostedGpuCounters Counters() const;
    void Close();
private:
    struct Resources;
    Resources *resources_ = nullptr, *cpu_ = nullptr;
    FhdFlashProbe *probe_ = nullptr;
    HostedGpuCounters counters_{};
    UINT lastWidth_=0, lastHeight_=0;
    SdlGpuMailbox *mailbox_ = nullptr;
    HANDLE file_ = INVALID_HANDLE_VALUE;
    LONG accepted_ = 0, failedGeneration_ = 0;
    const char *stage_ = "idle";
    HRESULT Connect(ID3D11Device *device, ID3D11DeviceContext *context, LONG generation);
    void Disconnect();
};
