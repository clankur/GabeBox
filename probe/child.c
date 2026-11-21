/* Packaged child used only by the parent UWP process-creation probe. */
typedef unsigned int UINT;
#define WINAPI __stdcall

__declspec(dllimport) void WINAPI ExitProcess(UINT exitCode);

static UINT ExpectedExitCode(void) {
    return 0x51A7u;
}

/* Retain a real base relocation even in this tiny position-independent x64 image. */
static UINT (*volatile exitCodeAtEntry)(void) = ExpectedExitCode;

void WINAPI ChildEntry(void) {
    ExitProcess(exitCodeAtEntry());
}
