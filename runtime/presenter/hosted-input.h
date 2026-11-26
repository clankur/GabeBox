#pragma once
#include "../shared/sdl-host-protocol.h"
#ifdef __cplusplus
extern "C" {
#endif
/* The render/view thread owns this bridge. The WWA input writer must be
   suspended before Begin. End neutralizes input before WWA resumes writing. */
HRESULT FhdBeginHostedInput(SdlHostMailbox *box, void *coreWindow);
void FhdSetHostedInputProfile(BOOL gamepad, UINT gameWidth, UINT gameHeight);
void FhdHostedMenuOnRelease(BOOL enabled);
BOOL FhdHostedMenuHeld();
/* Menu + View chord diagnostics since Begin: accepted chords by source (the live
   Windows.Gaming.Input reading or VK_GAMEPAD key events), both buttons down without
   a paired press, and pending chords dropped because a button stayed held. */
typedef struct FhdHostedChordCounts {
    UINT pad, keys, ignored, expired;
} FhdHostedChordCounts;
void FhdReadHostedChordCounts(FhdHostedChordCounts *out);
void FhdPauseHostedInput(void);
BOOL FhdHostedInputForeground(void);
/* TRUE while an overlay request or a covered panel holds the game's input. */
BOOL FhdHostedInputPaused(void);
/* CoreWindow key events (KeyDown/KeyUp) received since Begin, in any input state. Remote
   controller input, the keyboard and the remote Menu + View chord all arrive this way, so a
   count that stays put while keys are sent to a shown game means the window lost key focus. */
UINT FhdHostedKeyEvents(void);
/* With the native controller profile: the serial of the first controller sample published
   after the latest FhdResumeHostedInput (0 until it is published, and 0 with the pointer
   profile), and the serial the game last acknowledged. An acknowledgement at or past that
   serial shows that the shown game polled hosted input after the resume. */
void FhdReadHostedResumeSample(LONG *serial, LONG *ack);
int FhdPollHostedInput(SdlHostMailbox *box, void *coreWindow); /* 1 requests library overlay */
/* On the view thread following a switch back or a failed switch away. Clears held and
   suppressed chord state and republishes the next controller sample under a new serial. */
void FhdResumeHostedInput(void);
void FhdEndHostedInput(void);
#ifdef __cplusplus
}
#endif
