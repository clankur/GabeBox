# Spider-Man diagnostic loader (November 26)

This folder is a source snapshot of the diagnostic `XboxWin32LoaderTrace.dll` used for the Spider-Man trials `1.0.271.205` through `1.0.273.220`, which are recorded in [the compatibility note](../spider-man-compatibility.md). The loader started from the `.315` world-entry loader and added the headers below. The snapshot is kept so the trials can be reproduced. It is not a shared component.

| File | Purpose |
| --- | --- |
| `lazy.h` | Commits large engine pools on demand: it reserves the full pool, commits 64 KiB chunks on first touch through a vectored exception handler, and commits `ReadFile` destinations inside a pool before the read. This is the change that restored the world. It is the candidate for a reusable runtime component. |
| `owners.h` | Attributes committed CPU memory and D3D12 heaps to the code that requested them. |
| `allocs.h` | Hooks D3D12 heap and resource creation. Also reuses a successful `GetDeviceRemovedReason` result for up to 8 ms. |
| `census.h` | Records thread, memory and timer measurements, the ray-tracing gate bytes, and the affinity experiment. |
| `profile.h` | Sampling CPU profiler with unwinding to the first game frame. |
| `gpuprof.h` | Records GPU timestamps around each `ExecuteCommandLists` call on the game's queues. |
| `drive.sh` | Waits for the main menu, presses **Continue** through remote input, and keeps copying the trace. |
| `fallback-angles.md` | Ranked fallback options for freeing memory. |

## Build

Cross-compile with clang for x86_64-w64-windows-gnu, using zig's MinGW headers and the UWP `WindowsApp.lib`. The commands are in the note's "Local native cross-build" section.

Then:

1. Assemble the layout with `build_tools/prepare_spider_world_recovery.py`.
2. Replace `XboxWin32LoaderTrace.dll` in the layout with this build.
3. Replace `Fhd.Native.dll` and `Fhd.Native.winmd` with the STS2 presenter build from run `45769234415`, so remote controller input works.
