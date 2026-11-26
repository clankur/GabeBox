/* Direct3D 12 fence-wait trace, linked into the loader-trace adapter. See
   d3d12-fence-trace.c for the contract and d3d12-fence-policy.h for the
   package-root file. */
#pragma once
#include <windows.h>

/* Reads xbox-d3d12-fence-trace.txt from the package root. Without the file
   it does nothing and writes nothing. With it, writes the selected mode to
   the loader trace and, unless the file says off, starts the background
   thread that installs the hooks once d3d12.dll is loaded. Once per
   process. */
void XboxD3D12FenceTraceStart(void);

/* Patches, through this device, the shared ID3D12Fence table
   (SetEventOnCompletion, slot 9) for plain and shared fences, the shared
   ID3D12CommandQueue table (ExecuteCommandLists, slot 10) for a direct
   queue, and the device's CreateCommandQueue (8) and CreateFence (36), so a
   fence or queue with a table of its own is patched when it is created.
   S_FALSE when this device's table is already patched. */
HRESULT XboxD3D12FenceTraceInstall(void *device);

/* After the install: signals 1 on a fence from a direct queue of its own and
   waits for it with the original SetEventOnCompletion(1, NULL), which the
   counters do not see. S_FALSE when that wait returned before the fence
   reached 1. waitUs receives how long the wait took. */
HRESULT XboxD3D12FenceSelfTest(void *device, ULONGLONG *waitUs);
