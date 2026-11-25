#pragma once
#include "../shared/sdl-host-protocol.h"
#include "../shared/sdl-rumble-protocol.h"
#ifdef __cplusplus
extern "C" {
#endif
/* The render/view thread owns this bridge. The WWA input writer must be
   suspended before Begin. End neutralizes input before WWA resumes writing. */
HRESULT FhdBeginHostedInput(SdlHostMailbox *box, SdlRumbleMailbox *rumble, void *coreWindow);
void FhdSetHostedInputProfile(BOOL gamepad, UINT gameWidth, UINT gameHeight);
void FhdUseRoutedPointer(void);
void FhdHostedPointerUpdate(double x, double y, LONG buttons, LONG wheelDelta, BOOL horizontalWheel);
void FhdHostedPointerRelease(BOOL exited);
void FhdHostedMenuOnRelease(BOOL enabled);
BOOL FhdHostedMenuHeld();
void FhdPauseHostedInput(void);
void FhdStopHostedRumble(void);
BOOL FhdHostedInputForeground(void);
int FhdPollHostedInput(SdlHostMailbox *box, void *coreWindow); /* 1 requests library overlay */
void FhdResumeHostedInput(void); /* on the view thread following a switch back or a failed switch away */
void FhdEndHostedInput(void);
#ifdef __cplusplus
}
#endif
