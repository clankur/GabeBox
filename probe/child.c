/* Packaged child used only by the parent UWP process-creation probe. */
typedef unsigned int UINT;
#define WINAPI __stdcall

__declspec(dllimport) void WINAPI ExitProcess(UINT exitCode);

void WINAPI ChildEntry(void) {
    ExitProcess(0x51A7u);
}
