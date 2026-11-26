/* Direct3D 12 residency counters and the eviction switch, linked into the
   loader-trace adapter. See d3d12-residency.c for the contract and
   d3d12-residency-policy.h for the package-root file. */
#pragma once
#include <windows.h>

/* Reads xbox-d3d12-residency.txt from the package root, writes the selected
   mode to the loader trace and, when the mode calls for hooks, starts the
   background thread that installs them once d3d12.dll is loaded. Once per
   process. pipelineHooks: whether the pipeline hooks (which create a device
   of their own) are running; the default mode installs the counters only
   then. */
void XboxD3D12ResidencyStart(BOOL pipelineHooks);

/* Patches the shared ID3D12Device method table through this device:
   MakeResident (34), Evict (35), ID3D12Device3::EnqueueMakeResident (50)
   and, where the runtime implements ID3D12Device15,
   RegisterTrimNotificationCallback (83). Idempotent. */
HRESULT XboxD3D12ResidencyInstall(void *device);
