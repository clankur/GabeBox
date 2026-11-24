#pragma once
#include <unknwn.h>
#include "../shared/sdl-rumble-protocol.h"

// The native view thread owns the mailbox and controller reference. It
// invalidates the command generation before releasing or changing a pad.
class HostedRumble {
    struct Vibration {
        double leftMotor, rightMotor, leftTrigger, rightTrigger;
    };
    static_assert(sizeof(Vibration) == 32, "IGamepad vibration ABI");

    SdlRumbleMailbox *box_ = nullptr;
    IUnknown *pad_ = nullptr, *identity_ = nullptr;
    DWORD motors_ = 0;
    unsigned failedStops_ = 0;
    bool attempted_ = false, stopping_ = false;

    static HRESULT Apply(IUnknown *pad, DWORD motors) {
        using SetVibration = HRESULT(STDMETHODCALLTYPE *)(IUnknown *, Vibration);
        const Vibration value{LOWORD(motors) / 65535.0, HIWORD(motors) / 65535.0, 0., 0.};
        return reinterpret_cast<SetVibration>((*reinterpret_cast<void ***>(pad))[7])(pad, value);
    }

public:
    void Attach(SdlRumbleMailbox *box) {
        StopBeforeSuspend();
        box_ = box;
        if (box_) SdlRumbleDisarm(box_);
    }

    void StopBeforeSuspend() {
        // A hidden or suspended view may never receive another frame.
        while (pad_) Stop();
    }

    void Stop() {
        if (pad_) {
            SdlRumbleDisarm(box_);
            stopping_ = true;
            // A transient stop error must not send the old command to a
            // replacement. Retain the pad for retries, with a bounded close.
            if (motors_ && FAILED(Apply(pad_, 0)) && ++failedStops_ < 3) return;
            pad_->Release();
            identity_->Release();
            pad_ = nullptr;
            identity_ = nullptr;
        }
        motors_ = 0;
        failedStops_ = 0;
        attempted_ = false;
        stopping_ = false;
    }

    void Update(IUnknown *pad) {
        if (!box_) return;
        if (!pad || stopping_) {
            Stop();
            return;
        }
        if (pad_ != pad) {
            IUnknown *identity = nullptr;
            if (FAILED(pad->QueryInterface(__uuidof(IUnknown), reinterpret_cast<void **>(&identity))) || !identity) {
                Stop();
                return;
            }
            if (identity == identity_) {
                identity->Release();
            } else {
                Stop();
                if (pad_) {
                    identity->Release();
                    return;
                }
                pad->AddRef();
                pad_ = pad;
                identity_ = identity;
            }
        }
        if (!attempted_) {
            // The virtual slot becomes writable only after this pad accepts a stop.
            attempted_ = true;
            if (SUCCEEDED(Apply(pad_, 0))) SdlRumbleArm(box_);
        }
        if (!SdlRumbleGeneration(box_)) return;
        const DWORD requested = SdlRumbleRead(box_);
        if (requested == motors_) return;
        if (FAILED(Apply(pad_, requested))) {
            if (requested) motors_ = requested;
            Stop();
            return;
        }
        motors_ = requested;
    }
};
