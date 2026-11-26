# Mesa UWP runtime with a persistent shader cache

The OpenGL route runs Godot GLES3 (and other desktop GL titles) on Mesa's d3d12 Gallium driver from [mesa-uwp `34732bb`](https://github.com/XboxEmulationHub/mesa-uwp/tree/34732bbc207daea10bdf2fb969a4a0f7769ea1de) (Mesa 26.2.2 plus one UWP commit). The base package carries the maintainers' prebuilt [26.2.2 release](https://github.com/XboxEmulationHub/mesa-uwp/releases/tag/26.2.2). That build has no shader disk cache, so every GLSL program and every d3d12 shader variant is compiled again on every launch, at its first use.

This component pins the same revision, adds a patch series that gives the d3d12 driver a persistent cache, and builds it in CI. The packager can then swap the runtime in, and a profile marker turns the cache on for a title. Without the marker, or without `--mesa-runtime`, nothing changes.

| File | Purpose |
| --- | --- |
| [`mesa-uwp.json`](mesa-uwp.json) | Pins the source revision, the license hash, the prebuilt release, the UWP Meson fork, the Python and bison/flex versions, and each patch's SHA-256 and build variant |
| [`uwp-x64.ini`](uwp-x64.ini) | Meson machine file: `subsystem = 'uwp'` (AppContainer projects, `WINAPI_FAMILY_APP`) |
| [`patches/`](patches) | The patch series against `34732bb`, applied with `git apply` in order |
| [`.github/workflows/xbox-mesa-uwp.yml`](../../.github/workflows/xbox-mesa-uwp.yml) | Windows build. Its artifact is pinned by the SHA-256 of its `SHA256SUMS` |

## Where Mesa's compile time goes (code map at `34732bb`)

Godot GLES3 compiles a shader specialization when it first needs one, so all of this runs on the render thread at first use:

1. **GLSL → NIR, at `glCompileShader` and `glLinkProgram`.** This covers GLSL parse and AST → IR, GLSL IR optimization, linking, `glsl_to_nir`, and st/mesa's NIR lowering and optimization loop. The program cache in `st_glsl_to_nir.cpp:446` (`st_load_nir_from_disk_cache`) and `can_skip_compile` in `glsl_parser_extras.cpp` would skip all of it, but only when `ctx->Cache` is non-null. `ctx->Cache` comes from `pipe_screen::get_disk_shader_cache`, which d3d12 never implements. The same hook gates `GL_ARB_get_program_binary` (`st_extensions.c:484`). With no formats, `GL_PROGRAM_BINARY_LENGTH` is 0 (`shaderapi.c:909`), which is why Godot writes empty cache entries.
2. **Driver-side NIR passes at shader creation.** `d3d12_create_shader` and `d3d12_create_shader_impl` lower subgroups, bit sizes and clip distances once per GL program stage. These passes are cheap compared with the rest.
3. **Variant selection at draw.** `d3d12_fill_shader_key` plus a linear scan of the selector's variants is cheap per draw. A new key clones the NIR and runs the variant passes: point sprites, polygon stipple, flat-shading emulation, depth range, `nir_lower_tex` and varying fixups (`select_shader_variant`, `d3d12_compiler.cpp:1053-1209`). Then it calls `compile_nir`.
4. **`compile_nir` → `nir_to_dxil`.** After a few more lowerings, `nir_to_dxil` (`nir_to_dxil.c` from about line 6560) runs I/O lowering and **two full `optimize_nir` loops**. It then lowers memory access, vectorizes, emits the DXIL module and writes LLVM bitcode. In the d3d12 backend this is the largest CPU cost per variant.
5. **DXIL validation and signing.** It runs for every variant on Xbox. The d3d12 context loads the packaged `dxil.dll` (`d3d12_context_common.cpp:703-706`, DXC v1.8.2505.1, pinned in `build_tools/stage_mesa_uwp.py`). `dxil_validate_module` calls `IDxcValidator::Validate(..., DxcValidatorFlags_InPlaceEdit)` (`dxil_validator.cpp:311`), which re-parses the bitcode, validates it and writes the hash D3D12 requires. Without the validator, D3D12 rejects the unsigned DXIL.
6. **Driver pipeline creation** (`CreateGraphicsPipelineState`) follows in the D3D12 runtime and Xbox driver. Mesa does not cache it. The repository's `pso` trace measured it at about 13% of the STS2 first-turn stall.

None of this is measured per stage on the console yet. Patch 0003 and the GL trace below exist to measure it.

## Decision: new code; there is nothing upstream to backport

- Upstream Mesa `main` (checked at `66e82ef1`, 2026-11-25) has **no d3d12 disk cache**. Neither `d3d12_screen.cpp` nor `d3d12_compiler.cpp` references `disk_cache` or `get_disk_shader_cache`.
- Upstream still refuses `-Dshader-cache=enabled` on Windows (`meson.build`: "Shader Cache does not currently work on Windows"). `src/util/disk_cache_os.c` still reads `/* TODO: implement disk cache support on windows */`. Rebuilding the pinned revision with that option fails at configure; it does not just produce a no-op.
- The [mesa9x](https://github.com/JHRobotics/mesa9x) Windows fork has the same TODO.

The series therefore reuses upstream's cache format, keys and GL front-end integration unchanged, and adds the two missing pieces:

| Patch | Change | Size |
| --- | --- | --- |
| 0001 | `with_uwp` from the machine file's subsystem. Needed because the published UWP Meson fork has no `uwp` builtin option | +3 −1 |
| 0002 | Windows backend for `util/disk_cache` (multi-file layout, same entry format). Uses only C runtime and AppContainer-safe Win32 calls. `-Dshader-cache=enabled` is allowed on Windows, while `auto` stays off | +427 −16 |
| 0003 | d3d12 `get_disk_shader_cache`, a DXIL variant cache in `compile_nir`, and `D3D12_SHADER_CACHE_STATS` counters | +309 −10 |

**Keys.** A variant's DXIL is a function of the NIR passed to `nir_to_dxil` and of the `nir_to_dxil_options`. The key is therefore the serialized pre-compile NIR plus every option field. The `d3d12_shader_key` is already reflected in that NIR through the passes it selects, and it holds pointers, so it is not hashed directly. The disk cache's driver id adds the Mesa version, git SHA and build time, the adapter IDs, the UMD version, and the capabilities that shape NIR and DXIL. Every new Mesa build therefore starts with a cold cache.

**Value.** The value is the validated, signed DXIL plus the NIR as `nir_to_dxil` left it. `nir_to_dxil` rewrites the NIR the driver keeps for bindings, root signatures and later stages. A hit restores both, so the code after it runs exactly as it does after a compile. Only DXIL that passed validation is stored. DXIL dumps, disassembly, `D3D12_DEBUG=experimental` (unvalidated) and verbose debugging bypass the cache.

**Opt-in.** The d3d12 cache is Windows-only. The screen creates it only when `MESA_SHADER_CACHE_DIR` (or `MESA_GLSL_CACHE_DIR`) is set and `DXIL_DEBUG` is not. A packaged process has no per-user default cache location. Without the variable, the patched build behaves like the prebuilt one: no cache and no program-binary formats. Linux/WSL builds of the driver keep upstream behaviour.

**The GL front end needs no extra code.** Once the screen returns a cache, st/mesa's own program cache skips GLSL compile, link and GLSL → NIR for programs it has seen, and program binaries get a format (`GL_PROGRAM_BINARY_FORMAT_MESA`). It does not skip the d3d12 variant compile, because the variant key depends on draw-time state. That is what the DXIL cache in 0003 is for.

**Godot 4.5.1's own GLES3 cache.** It needs only `glGetProgramBinary`/`glProgramBinary` and a non-zero `GL_PROGRAM_BINARY_LENGTH` ([`shader_gles3.cpp` `_save_to_cache`/`_load_from_cache`](https://github.com/godotengine/godot/blob/4.5.1-stable/drivers/gles3/shader_gles3.cpp#L544-L686)). It does not query `GL_NUM_PROGRAM_BINARY_FORMATS`. Two limits apply:

- It saves only the default specialization of each shader version, at version initialization. The specializations created at first use during gameplay are never written. Mesa's program cache and the DXIL cache cover those.
- Its cache key carries no driver identity, and a file that loads "successfully" with zero-length entries is never rewritten. **Delete the title's Godot `shader_cache` directory once** when switching to the patched runtime, or Godot keeps its empty entries.

## Build, package and enable

1. **CI.** Push to `runtime/mesa/**`, or dispatch "Build Mesa UWP runtime". The `shader-cache` job uploads `xbox-mesa-uwp-34732bb-shader-cache-x64` and prints its `SHA256SUMS` digest in the job summary. The `baseline` job builds with patch 0001 only. It compares the import and export names of all five DLLs with the pinned 26.2.2 release and fails if a DLL links the desktop C++ runtime.
2. **Package.** Run `python3 -m build_tools.prepare_big_picture_library ... --mesa-runtime <artifact dir> <SHA256SUMS sha256>`. This replaces `opengl32.dll`, `libgallium_wgl.dll`, `libEGL.dll`, `libGLESv2.dll`, `z-1.dll`, `LICENSE.mesa-uwp` and `mesa-uwp-source.txt` in the base layout. Leave the option out to keep the prebuilt `34732bb` pin.
3. **Enable per title.** Add an empty `steam-win32-mesa-shader-cache-<appid>.txt` to the build-matched profile, next to an `opengl3` trial profile. Staging refuses it unless the layout's `libgallium_wgl.dll` is a shader-cache build and `ProbeNative.dll` sets the variables. The launcher then sets, for that child only:
   - `MESA_SHADER_CACHE_DIR = LocalState\SteamWin32TrialProfiles\<account>\<appid>\MesaShaderCache`
   - `MESA_SHADER_CACHE_MAX_SIZE = 512M`, enforced by LRU eviction
   - `D3D12_SHADER_CACHE_STATS = LocalState\mesa-shader-cache-<appid>-<launch>.txt`

   Its launch report records `steam_shadow_mesa_shader_cache=profile` and the stats file name. Removing the marker restores the old behaviour. The cache folder persists across launches by design; delete it to reset.
4. **Measure (optional).** An empty `steam-win32-gl-trace-<appid>.txt` needs a `loader-trace` profile. It sets `XBOX_WIN32_GL_TRACE=1` and the existing `XBOX_WIN32_PSO_TRACE=1`, without the CoreCLR JIT trace. The hosted WGL adapter (`runtime/win32/window-gl.c`) then wraps `glCompileShader`, `glLinkProgram`, `glProgramBinary`, `glDrawArrays`, `glDrawElements` and their instanced forms. It writes `gl kind=<compile|link|binary|draw> utc=… us=… count=… total_us=…` for every call of 2 ms or more, beside the `pso kind=… us=…` lines. `total_us` includes the shorter calls, so the Mesa share of a stall window is the `total_us` difference across it minus the `pso` time inside it. A slow draw includes the variant compile and the pipeline creation it triggers.

## Verification status

Verified off-console:

- The patches apply in order to the pinned files and reproduce the edited tree. They were generated from those exact files.
- Patch 0002's Windows backend and the patched `disk_cache.c` pass a mingw-w64 `clang -fsyntax-only` check with `WINAPI_FAMILY_APP`. The remaining diagnostics come from the mingw environment (`timespec`, perfetto) and appear identically for the unpatched file. The POSIX path shows the same diagnostics before and after the patch.
- `window-gl.c` compiles with `-ffreestanding -Wall -Wextra -Werror` against mingw-w64 headers.
- The packager, staging and pin tests pass (`tests/test_mesa_runtime.py`).
- A separate review agent read both patches against the pinned headers. It found no compile or hit-vs-miss errors. Its three actionable findings are fixed:
  - only the final cache directory must be creatable;
  - `DXIL_DEBUG` disables the cache;
  - non-Windows builds are warning-free.

Verified in CI (run 45715041407, commit `f33948e`, windows-latest with VS 18 / MSVC 14.52):

- **Both variants build.** They use the Ninja backend with the UWP machine file. The pinned prebuilt's linker is 14.51, which is also VS 18. The fork's Visual Studio backend can't build Mesa: it generates Store projects with /ZW, PCH and XAML compilation. The build has no Vulkan driver (`-Dvulkan-drivers=[]`), since only the five GL libraries are packaged.
- **The shader-cache artifact** is `xbox-mesa-uwp-34732bb-shader-cache-x64`, with `SHA256SUMS` SHA-256 `3a4e806c33945469d57e23096ed5275025e0a50dc23b576c137e4cfceb7988e1`.
- **The baseline matches the 26.2.2 release's exports exactly** in all five DLLs, including every WGL and GL entry point in `opengl32.dll`.
- **19 import differences from the release.** None is a desktop-only API or a new module outside the API sets:

  | Difference | DLLs |
  | --- | --- |
  | `IsProcessorFeaturePresent` from `api-ms-win-core-processthreads-l1-1-1` (newer CRT startup) | all five |
  | `CreateEventA` in place of `CreateEventW`. The Store projects define `UNICODE`; Ninja does not. Same API set; Mesa passes no event name | `libgallium_wgl`, `libEGL` |
  | No `copysign`, `fmin` or `fmax` (float and double) imports from the CRT; the compiler now inlines them | `libgallium_wgl` |

- **The shader-cache build adds only app-container imports to `libgallium_wgl.dll`:**
  - CRT file and time calls;
  - `CreateFileMappingFromApp`, `MapViewOfFileFromApp` and `UnmapViewOfFile`;
  - zlib `deflate`/`inflate` from the packaged `z-1.dll`.
- The workflow now fails on any export difference, and on any new imported module other than an `api-ms-win-*` set or `z-1.dll`.

Not verified (needs the console):

- **Mesa sources and host.** The d3d12 sources compiled in CI. `ProbeNative.cpp` and `window-gl.c` build in the separate Xbox probe workflow.
- **The cache's calls.** Whether `CreateFileMappingFromApp` and the CRT file calls work in the game process's container.
- **Environment variables.** Whether `GetEnvironmentVariableA` sees the launcher's variables inside Mesa. No Mesa variable has been used on the console before.
- **Results.** Cache hits, and the size of the stall reduction.

## Live test recipe (STS2, AppID 2868840, build 26655341)

Package two runtimes: A uses the prebuilt Mesa with the GL trace marker; B uses `--mesa-runtime` with the GL trace and Mesa shader cache markers. Then, for B:

1. Delete `…\MesaShaderCache` and the title's Godot `shader_cache` folder. Launch, Continue, and play the first turn (first STRIKE, then an enemy turn). Quit to the library. This is the cold run.
2. Launch again and repeat the same path. This is the warm run.

Acceptance:

- The warm run's `mesa-shader-cache-2868840-*.txt` has `hits` > 0 and `misses` near 0, against all misses on the cold run.
- `nir_to_dxil_us` and `validate_us` fall to near 0 on the warm run.
- On the warm run, `gl kind=link` `total_us` drops sharply (program-cache hits) and `gl kind=draw` total time falls.
- `python3 -m tools.measure --require-gpu-60` on the first-turn window shows fewer and shorter gaps than run A and the cold run.
- Gaps left over should line up with `pso` lines (driver pipelines, not cached here) or with JIT.

Record the cache folder size after each run.

## Risks

- **Stale or corrupt entries.**
  - Each entry is CRC-checked and keyed on its full input. The driver id changes with every Mesa build.
  - A collision or a bad entry would need the same 256-bit key.
  - A deserialization failure falls back to compiling.
- **Concurrent writers.** The `.tmp` exclusive share mode plus a non-replacing `rename` mirrors the POSIX `flock` protocol. A crash leaves at most a stale `.tmp`, which the next writer reuses. The index file is mapped shared, as on Linux.
- **Disk use** is bounded by `MESA_SHADER_CACHE_MAX_SIZE` (512 MiB) per account and title, on the app's install volume.
- **Behaviour change** once program binaries exist. Godot starts writing real binaries, and Mesa rejects binaries from another build (driver hash check), so Godot recompiles them.
- **Shared state.** The `pso` trace and the PSO costs are unaffected. A driver-side `ID3D12PipelineLibrary` cache is separate work.
- **NIR after `nir_to_dxil` is serialized**, which upstream never does. A deref left pointing at a uniform struct that `nir_to_dxil` removed would assert in `nir_serialize`. GL lowers those structs earlier, so this is not expected. A `NIR_DEBUG=validate` debug build would confirm it.
- **Build identity.** It comes from `MESA_GIT_SHA1`, `__DATE__` and `__TIME__` in `d3d12_screen.cpp`. A clean CI build always changes it. An incremental local build that touches only the compiler sources would not, and would reuse old entries, so do not ship incremental builds.
- **Lost entries at exit.** Writes go through a background queue. A process that exits without destroying the screen can lose its last few entries, and the stats file can lag by up to 1 s. Expect "misses near 0" on a warm run.
- **Path length.** Entry paths are the LocalState path plus about 150 characters. A failure past `MAX_PATH` shows up only as misses.
- **Build tests.** `-Dbuild-tests=true` with the cache adds upstream's POSIX-only `cache_test.cpp`. The workflow does not build tests.
- **Fork maintenance.** The patches touch `util/disk_cache*` and the d3d12 compiler. A mesa-uwp rebase needs them re-applied and re-pinned.
