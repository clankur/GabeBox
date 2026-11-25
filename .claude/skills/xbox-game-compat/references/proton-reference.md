# Mining Proton and Wine for Xbox adapters

Proton runs Windows games on Linux by composing reusable components and selecting them
per title. This layer does the same thing in a different host: a Windows x64 UWP
process on retail Xbox hardware, with Direct3D 11 and 12 available natively, an app
lifecycle instead of a desktop, and a documented foreground memory budget. So Proton is
not a source to copy from — it is the best available *specification* of what each Win32
contract has to do, including the error paths games actually depend on.

Contents: how to use the lookup · component map · what Xbox supplies instead ·
reading an implementation for its contract · reuse and licensing · boundaries.

## How to use the lookup

```sh
S=.claude/skills/xbox-game-compat/scripts/upstream-ref.sh
$S ImmGetIMEFileNameA                       # symbol → Wine module via the .spec files
$S CM_Register_Notification --module dlls/cfgmgr32
$S --repo proton --path proton --grep appid # per-title configuration in the proton script
$S --repo dxvk --grep 'D3D11CreateDevice'
```

The first call for a tree clones it blobless and sparse (about a second, a few MB) under
`${XBOX_COMPAT_CACHE:-~/.cache/xbox-game-compat}`; later calls reuse it. The output ends
with a permalink pinned to the commit that was read — use that form in `analysis/`
notes, never a branch-relative link, because line numbers move.

Default trees: `ValveSoftware/wine` and `ValveSoftware/Proton` at `proton_11.0`, DXVK
and vkd3d-proton at `master`. Pass `--ref` for another tag. Upstream `wine-mirror/wine`
is also worth consulting for history on a function Valve has not patched.

## Component map

| Failing behavior on Xbox | Where Proton answers it | Start here |
| --- | --- | --- |
| A Win32 API is absent or stubbed | Wine's implementation of that DLL | `$S <Symbol>` — the `.spec` files resolve symbol → module |
| Desktop window classes, message loop, monitors | `dlls/user32`, `dlls/win32u/sysparams.c` | window registration, class lookup, `EnumDisplay*`, `GetSystemMetrics` |
| ANSI/Unicode pairs (`*A` over `*W`) | the ANSI wrapper beside the Unicode entry | e.g. `dlls/imm32/imm.c`, `dlls/user32/sysparams.c` |
| DXGI swap chains, present validation | `dlls/dxgi/swapchain.c` | which presents are rejected, and with what HRESULT |
| Direct3D 11 behavior and feature levels | `dlls/d3d11`, `dlls/wined3d` | device creation and capability reporting |
| Direct3D 12 | `vkd3d-proton` (`--repo vkd3d`) | mostly a translation target; Xbox has native D3D12 |
| Controller state and vibration | `dlls/xinput1_3/main.c` | slot semantics, two-motor mapping, capability reporting |
| Device arrival and removal notifications | `dlls/cfgmgr32/main.c` | callback payload layout and lifetime |
| HID devices | `dlls/hidclass.sys`, `dlls/winebus.sys` | what a game sees when enumerating |
| Audio endpoints and WASAPI | `dlls/mmdevapi/devenum.c` | enumerator and endpoint COM contracts, empty-property behavior |
| Profile, roaming and documents folders | `dlls/shell32/shellpath.c` | `SHGetFolderPathW` flags and creation semantics |
| Registry expectations of middleware | `dlls/advapi32`, `dlls/kernelbase` | key layout a game's SDK probes |
| Steamworks initialization and the client pipe | Proton's `lsteamclient` and the `proton` script's Steam setup | versioned SDK ABI headers, how the client is located |
| Per-title overrides and launch configuration | the `proton` script's per-AppID configuration | the model for declarative, reversible title overrides |
| 32-bit titles | Proton's wow64 path and `FEX` | an x86 guest, not an adapter |

Proton's `lsteamclient/steamworks_sdk_*` directories are also the most convenient
authoritative copy of the versioned Steamworks headers when an interface version has to
be matched exactly.

## What Xbox supplies instead

Do not port the Linux mechanism; port the contract onto what the console already has.

| Proton component | Xbox equivalent in this layer |
| --- | --- |
| Wine's win32u + X11/Wayland driver | the app's own XAML host, virtual window and monitor in the Win32 window adapter |
| DXVK (D3D9/10/11 → Vulkan) | native Direct3D 11 plus the DXGI adapter that turns desktop swap chains into composition swap chains and shared GPU textures |
| vkd3d-proton (D3D12 → Vulkan) | native Direct3D 12 |
| Mesa on Vulkan for GL titles | Mesa's D3D12 backend, surfaceless EGL, and the packaged desktop GL path |
| Wine's XInput over evdev/SDL | one foreground controller source translated into a shared host mailbox that XInput and SDL callers both read |
| Wine's mmdevapi over PulseAudio/ALSA | UWP audio activation, with a packaged adapter for enumeration results the console does not provide |
| The Wine prefix as the writable world | app-local storage, per-account profile directories, and process-local registry mappings |
| `proton` script environment overrides | signed, build-matched profile files and markers staged into the package |
| Wine's loader and DLL overrides | per-function import recipes bound to the audited image's size and SHA-256 |

The through-line: Proton gets to patch a whole user-space OS, while this layer gets to
redirect named imports in a verified copy of one game's binaries. That constraint is why
adapters must preserve calling convention, return values, ownership and error behavior
exactly — the rest of the game's code is unchanged and still believes it is on Windows.

## Reading an implementation for its contract

What you want from an upstream function is rarely the algorithm. It is:

1. **The failure contract.** What does it return when the feature is unavailable, and
   does the caller survive that? Wine's ANSI IME lookup returning zero when there is no
   IME filename is a complete answer for a console with no IME, because the game checks
   for zero.
2. **Ownership and lifetime.** Who frees the buffer, how long a registration lives, what
   an unregister must drain.
3. **Argument handling.** Null buffers used as length queries, flags that change
   semantics, the ANSI/Unicode conversion behavior.
4. **Which errors are expected.** Games branch on specific errors; returning a plausible
   but different one sends the engine down an untested path.

Then confirm the game's side with REA: the call site, the arguments it passes, and what
it does with each outcome. An adapter is correct when those two descriptions agree.

Engine sources are reference material on equal footing — SDL for video, GL and
controller paths, Godot for Windows display initialization and capability checks, Mesa
for the GL backend, libGDX and MonoGame for managed games. When a trial reaches an
engine's own log message, read that engine's source at the pinned version before
guessing.

## Reuse and licensing

Treat upstream as a specification by default and implement against Xbox APIs. When a
component is genuinely reused rather than re-implemented, follow what this repository
already does for SDL, Mesa and the QR renderer: pin the exact upstream version, keep the
license file with the component in the package, verify the artifact's checksum and
exports before staging, and record the version in the compatibility note. Check each
file's own header and the project's LICENSE rather than assuming — Wine is LGPL-2.1+,
DXVK is zlib-licensed, vkd3d-proton is LGPL-2.1, and the Proton tree mixes licenses
across its submodules. Copying a snippet out of an LGPL source file into a signed
package is a licensing decision, not a coding shortcut; if that is what a fix requires,
say so and let the user decide.

## Boundaries worth naming early

Some gaps are not adapter-shaped, and recognizing them early saves a week:

- **x86 binaries — but read the COR header before concluding that.** The console runs
  x64, and for native 32-bit machine code Proton's answer is wow64 or FEX, which here
  means a guest runtime with its own feasibility question. A managed image is a
  different case that looks identical in the COFF machine field: `inspect-game` reports
  `pe_machine`, `clr_flags` and `clr_requires_x86`, where the requirement holds only
  when `32BITREQUIRED` is set and `32BITPREFERRED` is not. An `ILONLY` assembly that
  merely declares a 32-bit preference contains no x86 machine code at all, so the
  routes are a publisher x64 or AnyCPU build, or re-hosting the IL on a packaged x64
  managed runtime — the shape the hosted managed adapters already use — not emulation.
  Steam's own `Invalid platform` refusal (result `29`) happens before any adaptation is
  reached, so it describes the launch route rather than the image's real requirement.
- **Kernel-mode components.** Anti-cheat drivers, kernel middleware and services cannot
  be installed by a sideloaded package. Publisher-supported options that disable them
  (an EAC-disabled launch option, for instance) are the practical route.
- **Managed native entry points.** A C++/CLI library with managed vtable fixups needs a
  rebuild upstream; there is no import redirect that supplies `mscoree!_CorDllMain`.
- **Memory budget.** The foreground budget is finite and documented; a title that needs
  more is a measurement result, not a missing API.

Before calling any of these a blocker, record the alternatives tested and the specific
runtime limitation that remains — that is what `AGENTS.md` asks for, and it is what
makes a blocker re-checkable when the platform changes.
