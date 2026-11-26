# Fallback angles: Spider-Man (1817070 / 23986256) world zones under the 5 GiB commit limit

From the planning subagent, 2026-11-26. "Verified" means it was read from the exe, the recipes or the analysis note. "Inference" means it is unverified.

## Facts used by several angles
- **F1. Menu AssetHeap demand is fixed at ~319.9 MiB.** Used bytes before the menu allocation were 335,465,840 in .292 and 335,465,832 in .293 (analysis note lines 203 and 205). The world needs more than 368 MiB. The lazy-828 run measured at least 448 MiB at world entry before it ran out of commit.
- **F2. The texture and model recipes did not change the arena block count:** 7,047, 7,047, then 7,045.
- **F3. The reference runtime TOC routes 12 archives; we use 3** (mip, zone, models). Column 48 is `local_distant_xyhalf`: 15 assets, 73,212,020 bytes.
- **F4. AssetHeap allocation path.**
  - Pool object at 0x07785B50. The wrapper at 0x017570B0 calls 0x01C5D2F0(pool, size, name, align): cache at pool+0x48, alloc at ops+0x18, free at ops+0x20.
  - Getter at 0x01757100 (50 callers).
  - Named callers include LightGridStreamer (0x01725D00), Bink Movie Frames, NavSystem TileCache, Dynamic Decal Heap, VFX vector fields and zone Heap Set (0x01A1EE1D).

## 1. Distant-light archive (local_distant_xyhalf) — about 210 MiB
- **What it is.** Five 812×1472 RGBA8 arrays with 12 slices each (57,372,800 bytes apiece), plus R8 masks and 164-byte headers. Halving width and height (reference ResampledTexture.cs, DirectXTex Resize LINEAR|FORCE_NON_WIC|SEPARATE_ALPHA) takes them from 292,841,780 to 73,212,020 bytes.
- **Evidence.** All 15 rows are routed in the reference's runtime-toc.tsv; none are in our trials.
- **Inference.** 5 × 58.57 MB would fit inside the menu's 335 MB AssetHeap demand.
- **Diagnostic first.** Log the largest used arena blocks (≥8 MiB) in SampleAssetArena, and log any 812×1472 or 406×736 resources in the D3D12 hooks.
- **Recipe step.** Convert the 15 rows (the Xbox CPU helper used for the zone controls, ConvertPixels mode 2), append them to the prepared archive, and add the 15 TOC rows.
- **Pass condition.** Free space in the menu arena rises from 13.6 MB to about 230 MB, and the world reports zero detail-2 zone failures.

## 2. AssetHeap owner census by name
- Wrap the ops slots for alloc (+0x18) and free (+0x20) to track live KiB per name string. Emit the top 40.
- LightGridStreamer fails quietly (0x01725D9B). Its slot count is min(dword 0x7A89228, dword 0x7A8922C), which a later step could cap.

## 3. Turn off ray-tracing setup when RT is off — 80–200 MiB
- The byte at 0x7A89150, read at 0x0188A2B3 and 0x0188A2C3, gates RayTracingManager::Initialize (0x01856410). The game log shows "BVH heaps 80MB" with RT off.
- **Log first.** Record the gating bytes.
- **Overlay.**
  - 0x0188A2B3 (file 0x18898B3): `0F B6 0D 96 EE 1F 06` → `31 C9 0F 1F 44 00 00`
  - 0x0188A2C3 (file 0x18898C3): `0F B6 0D 86 EE 1F 06` → `31 C9 0F 1F 44 00 00`
- **Risk.** The settings path at 0x018917AE can call Initialize again.

## 4. Config-driven temporary allocators — 60–120 MiB
- Asset Loader Temp size is dword 0x7A88848, read at 0x01622B16. A fixed 26 MiB allocation follows at 0x01622B55.
- Scratch sizes are qwords 0x7A87F38 and 0x7A87F40 (config keys ScratchSize and ScratchSizePersistent).
- Clamp them after config is parsed. Do this after angle 1, since the temp allocator may be sized for the largest asset.

## 5. Attribute the unexplained ~1 GB
- Measure process heaps with HeapSummary.
- Walk the private regions that no owner table claims, grouping them by allocation base.
- Record commit before and after steamclient64.dll loads and after SteamAPI_Init.
- The reference installs only a private steam_api64_real.dll and curated pipeline caches (8,435 graphics and 639 compute, against 10,937 stock).

## 6. Decommit free arena blocks — 50–200 MiB
- The arena header layout: head & ~3 = size, bit 0 = free, bit 1 = previous block free.
- After a free, decommit the interior 64 KiB chunks of free blocks ≥192 KiB. LazyFault recommits them on the next touch.

## 7. Cheaper D3D12 memory — 30–150 MiB
- (a) Clamp the QueryVideoMemoryInfo budget to CurrentUsage + 256 MiB so the texture streamer demotes.
- (b) Change the D3D12MA PreferredBlockSize from 16 MiB to 8 MiB at 0x018ECEFB (file 0x18EC4FB): `48 C7 44 24 78 00 00 00 01` → `... 00 00 80 00`.
- (c) Measure the slack in heaps ≤256 KiB.

## 8. Lower internal render resolution — 50–130 MiB
- RT/DS heaps grew about 300 MiB on world entry. This conflicts with the native 1080p goal.
