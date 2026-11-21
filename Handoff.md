# VeoxDMA (ROB RUN) — Living Handoff

> Maintained as a living reference synced with the codebase. Keep entries concise, factual, and tied to actual files/invariants.

---

## Workspace & Build Rules

- **Active repo / branch:** `c:\Users\jaspe\Documents\repos\robrun\_upstream` (`camera-low-latency`)
- **Solution:** `_upstream\VeoxDMA.sln` (`Release|x64` -> `_upstream\Build\Release\VeoxDMA\VeoxDMA.exe`)
- **Target machine:** Built on this PC, **run on 2nd PC only** — never launch `VeoxDMA.exe` locally.
- **Reference IL2CPP dump:** `c:\Users\jaspe\Documents\repos\robrun\_dump_new\dump.cs`

---

## How Stuff Works (Current Architecture)

### 1. Display & Overlay (`VeoxDMA/Overlay/Overlay.cpp`, `VeoxDMA/Input/InputManager.cpp`)
- Swapchain uses low-latency **flip-discard** (`DXGI_SWAP_EFFECT_FLIP_DISCARD`) with tearing support and a **1-frame latency limit** to minimize presentation queuing delay.
- Input polling runs off the render thread; basic screen-space culling avoids stalls on offscreen entities.

### 2. DMA & Memory (`VeoxDMA/Memory/Memory.cpp`, `VeoxDMA/Memory/Memory.h`, `RobRunReader.cpp`)
- MemProcFS initialized with `-norefresh` to eliminate background TLB/process-list stalls; manual `VMMDLL_OPT_REFRESH_FREQ_MEM` + `VMMDLL_OPT_REFRESH_FREQ_TLB` (`RefreshAllCaches`) and partial refreshes are triggered via rate-limited `MaybeRefreshDmaCaches` at zero-combat-stutter points:
  - **Idle / out-of-sight / far:** `PlayerEsp::DrawPlayers` publishes lock-free `(anyOnScreen, nearestOnScreenDist)` via `RobRunReader::ReportRenderState`. When nobody is on screen, nearest on-screen enemy is `> 80 m`, or the lobby is empty, `RefreshAllCaches` runs on a `2.5 s` cooldown.
  - **World worker partial expiry:** `RefreshMemCachePartial` every tick + `RefreshTlbCachePartial` every 4 ticks (~2.5–3 s) when quiet (`!anyOnScreen || nearest > 40 m`), every 10 ticks (~6–8 s) during close combat.
  - **Map change / respawn / teleport:** `RevalidateSession` checks live `Game -> GameModel -> BattleModel` via `ReadFresh`; `MarkSessionChanged` clears `rigCache` + `detailCache`; location-ID changes and `> 25 m` local-position teleports/respawns flush DMA caches and reset view/rig caches immediately.
  - **Bone-starvation safety net:** If nearby living enemies exist with `0` or `< 50%` skeletons for `>= 600 ms`, forces `RefreshAllCaches` (`2.5 s` cooldown), re-reads `_characterEntityViews`, and rebuilds bone-less rigs.
- Direct LeechCore physical read fast-path (`ReadPhysicalFast` + cached `VirtToPhys` translation) powers sub-ms camera polling.

### 3. Camera Pipeline (`VeoxDMA/SDK/CameraFinder.cpp`, `RobRunCamera.cpp`, `RobRunReader.cpp`)
- Dedicated high-frequency camera worker polls camera state independently of the heavier entity worker and publishes snapshots via a **lock-free seqlock** (`PublishNativeCamera` / `ReadNativeCamera`), eliminating render-thread mutex contention.
- **Native camera** (`CameraFinder`): uses `ReadFresh` for GOM/tag walks and physical fast-reads for the active view/projection block.
- **Managed camera** (`RobRunCamera::Read`): single-page physical fast-read (`VirtToPhys` cached 3 s, validated by finite position + unit-ish forward length) with `ReadFresh` fallback.
- **Dynamic FOV**: Cinemachine FOV is refreshed every `25 ms` (40 Hz) in `RobRunCamera::Read`, and native FOV every `8 ms` in `RefreshCameraStep`, preventing projection drift during sprints/vehicles/ADS.

### 4. Entity Worker & Batching (`VeoxDMA/SDK/RobRunReader.cpp`)
- Entity positions + poses are scatter-read in **small chunks** (max 4 pose-bearing entities / 12 entities per transaction) with `SwitchToThread()` yields between chunks and detail scatter phases so the camera thread can interleave sub-ms reads without multi-ms FPGA stalls.
- World/inventory dictionaries run on a separate background thread and publish immutable `std::shared_ptr` snapshots.

### 5. Skeleton Extraction & Pose Filtering (`VeoxDMA/SDK/SkeletonExtractor.cpp`, `RobRunReader.cpp`, `Esp/PlayerEsp.cpp`)
- **3-layer layout resolution** in `BuildLayoutFromView` (and `BuildLayoutByName`):
  1. **IK/Animation views:** `CharacterAnimationView` (`HeadView`, `TorsoView`) + `CharacterWithWeaponAnimationView` (`LeftHandIK` `+0x58`, `RightHandIK` `+0x60`, `LeftFootIK` `+0x68`, `RightFootIK` `+0x70`). Bottom-up IK arrays are auto-reversed; leg chains keep up to 4 joints; 1-bone junk slots are ignored.
  2. **Bone-name resolution (`ReadNamedBones` + `FillMissingAnchorsFromNames`):** Runs before finalizing anchors whenever any spine/leg/arm bone is missing and reuses the same name list for `out.boneMap` (chams) — zero extra DMA reads. Matches exact RobRun names (`M_spine_1_jnt..M_head_1_jnt`, `L/R_arm_1_jnt..3_jnt`, `L/R_leg_1_jnt..3_jnt`, `L/R_foot_1_jnt`) first, then Unity/Mixamo fallbacks, with parent-child ancestry checks.
  3. **Multi-hop parent-tree fallback:** If bone names are unavailable, walks **all** spine ancestors from `out.pelvis` up to `root` using world-space FK positions (`ComputeHierarchyWorldPositions`) to recover downward leg chains and upper-spine arm chains (supports single-leg/single-arm misses).
- **Name offset probing (`EnsureNameOffsetsProbed`) & scatter-batched name reads (`ReadNamedBones`):** Probes Unity's `Transform +0x30 -> GameObject +0x60/+0x48` first across up to 12 sampled bones via `Memory::ScopedScatter`, and reads all ~200 bone names in **3 scatter transactions (~1 ms)** instead of ~600 sequential reads (~250 ms). Skipped completely in `BuildLayoutFromView` when all limbs already resolved and `Chams` is off.
- **Leaf extension:** Restricted strictly to `out.leftFoot` / `out.rightFoot` children within `distSq < 0.06f` (~24 cm) so toes draw cleanly while hand-attached props (`Weapone1_jnt`, `Magazine1_jnt`) never sprout stray lines.
- **Budgeted layout rebuilds & 1-Hz fallback promotion (`RobRunReader.cpp`):**
  - Phase 3 skips `localEntityId`, dead players, and entities past `poseCullDist`, caps layout rebuilds/upgrades to **1 per worker pass**, keeps builds on cached `DmaRead` (scatter-batched names), and backs off `incomplete` upgrades (`500 ms -> 1500 ms -> 4000 ms`, max 4 attempts per view).
  - Phase 5 unmatched-entity fallback skips local/dead/far entities, caps to 1 per pass, and uses `Skeleton::FindEntityView` to promote found views straight into `cachedViewOwner` + `rigCache` (preventing players from getting stuck on 1-Hz `BuildSkeleton` refreshes).
- **Console performance telemetry (`RobRunReader.cpp`):** 3-second `[Perf]` line reports `worker` Hz, `avg`/`max` ms, per-phase `p1/p2/p3/det/p5` ms, `reb`/`fb` counts, and `skel=A/B`. Any single pass `>= 50 ms` logs a throttled `[SlowPass]` breakdown (`roots`, `views`, `p1`, `p2`, `p3`, `det`, `p5`, `match`).
- **Forward kinematics (`BuildPoseOutput`):** Uses parent's cumulative `worldScale[p]` (`world[b] = world[p] + Rotate(worldRot[p], worldScale[p] * local[b].p)`), matching Unity's transform evaluation.
- **Torn-pose rejection & 3-sample temporal median:** `BuildPoseOutput` rejects partial/torn poses (`result.size() < layout.segments.size()`). Accepted poses enter a 3-sample ring buffer in **pelvis-relative coordinates**, taking the per-component median and re-anchoring to the live pelvis position from the same read (erases 1-frame flickers with 0 ms positional lag).

### 6. ESP, Visibility & In-Game Map Overlay (`VeoxDMA/Esp/PlayerEsp.cpp`, `Radar.cpp`, `RobRunReader.cpp`)
- `HiddenAlpha` dimming is only applied when `pcfg.VisCheck || pcfg.WallCheck` is enabled (prevents idle/LOD-throttled characters from strobing grey via the animator-motion fallback).
- `IsValidBone` checks finite non-origin coordinates within `3.0 m` of `ent.position` (no minimum distance floor so feet planted on the root origin are never dropped).
- **In-Game Map Overlay (`Radar::DrawInGameMapOverlay`, `ReadInGameMapState`):**
  - Reads `BattleModel + 0x1D8` (`MapDialogModel.ScreenState` `+0x10`: `0=Closed`, `1=Opened`) and `BattleModel + 0x1E8` (`MapConfigModel`: `FullScreen` `+0x18`, `Minimap` `+0x20`).
  - Each `MapConfigVariance` provides live `Position` (`+0x10`), `Rotation` (`+0x18`), `CameraSize` (`+0x1C`), and `ResizeContainer` (`+0x20`, `UnityEngine.RectTransform`).
  - `ReadRectTransformUiBounds` resolves the transform index via `native + 0x30` (with chunked 4 KB pointer-table fallback), walks the ancestor chain via uncached `ReadFresh`, and evaluates Unity's native `UI::RectTransform` fields (`m_AnchorMin`, `m_AnchorMax`, `m_AnchoredPosition`, `m_SizeDelta`, `m_Pivot`) from the root `Canvas` down to `ResizeContainer` (`size = sizeDelta + parentSize * (anchorMax - anchorMin)`, `rect = (-size * pivot, size)`), cross-validated against `LocalTRS.translation.xy`.
  - Projects players (role dots + optional heading/names), cars & helicopters (vector icons), and optional dropped bags/valuables using the exact orthographic camera params and live 3D camera forward yaw (`GetFreshCamera`) for the rotating minimap, clipped via `PushClipRect`. Suppresses 3D ESP while the fullscreen `M` map is open when `Hide3DEspOnFullMap` is enabled.

---

## Recent Fixes Log

- **2026-11-21 — In-game minimap & fullscreen (`M`) map overlay polish, high-frequency sync, & startup hardening** (`RobRunSDK.h`, `RobRunReader.h/.cpp`, `WorldEntities.cpp`, `RadarConfig.h`, `Radar.h/.cpp`, `Main.cpp`, `AimController.cpp`, `VeoxMenu.cpp`)
  - Live `BattleModel.MapDialogModel` + `MapConfigModel` reader; fixed `ReadRectTransformUiBounds` (`rect=0(0,0..0,0)`) by evaluating Unity's 5-`Vector2f` `UI::RectTransform` layout (`anchorMin`, `anchorMax`, `anchoredPosition`, `sizeDelta`, `pivot`) across the Canvas hierarchy via `ReadFresh` and direct `native + 0x30` index resolution, with failure throttling (500 ms).
  - Added Canvas reference resolution (`uiScreenW x uiScreenH`) scaling to `ImGui::GetIO().DisplaySize`, fallback viewport bounds, tight minimap edge clamping (`[-0.98, 0.98]`), and live 3D camera horizontal forward rotation (`atan2(cam.forward.x, cam.forward.z)`) for the rotating minimap.
  - Native-style 2x icons: flat-notched rounded player chevron arrows (with crisp top outline), 2-pass seamless black-contoured car & helicopter silhouettes colored by driver role (red=criminal, blue=cop, green=civilian, cyan=teammate, silver=unoccupied), synced to live driver entity position (80–500 Hz) so driven vehicles never lag behind arrows. Player names shown on fullscreen (`M`) map only (`>= 15 px` with dark pill).
  - Maximum fullscreen range & refresh: merged `WorldEntitiesModel._battleCharacters` / `_notSpawnedBattleCharacters`, moved vehicles ahead of the 4096-entity loot cap, added 90 s faded last-known player ghosts beyond the replication grid, skipped 3D skeletons/BVH while fullscreen map is open, and polled fullscreen pan/zoom (`GetFreshFullMapVariance`) on the camera thread.
  - Startup hardening: serialized `Aim::StartAimWorker`/`StopAimWorker` & recoil worker lifecycle with `g_workerLifecycleMutex`, moved `ConfigFile::Load()` before `StartAttachLoop()`, added `SetConsoleCtrlHandler` for clean `VMMDLL_Close()`, and lowered GOM-failure cache flush + `FixCr3()` recovery to 20 attempts with throttled `[Reader]` console status.

- **2026-11-21 — 1-second bone rendering stalls & lag fixed + console telemetry added** (`SkeletonExtractor.h/.cpp`, `RobRunReader.cpp`)
  - Replaced ~600 sequential DMA reads in `ReadNamedBones` (and probing in `EnsureNameOffsetsProbed`) with **3 `Memory::ScopedScatter` calls** (~1 ms vs 250–500 ms per rig) and skipped name reads when all limbs resolved and `Chams` is off.
  - Replaced the 1-Hz `BuildSkeleton` fallback in Phase 5 (which ran once per second without caching the view/layout and didn't skip dead/local/far entities) with budgeted `Skeleton::FindEntityView` promotion into `cachedViewOwner` + `rigCache`.
  - Added Phase 3 distance/dead/local culling, `rebuildBudget = 1` per pass, cached `DmaRead` layout builds, and exponential upgrade backoff (`500 ms -> 1500 ms -> 4000 ms`, max 4 tries).
  - Added per-phase worker timing to `[Perf]` (`avg`/`max`/`p1/p2/p3/det/p5`/`reb`/`fb`/`skel`) plus a throttled `[SlowPass]` line whenever any pass takes `>= 50 ms`.
- **2026-11-21 — Post-map-change / full-respawn missing bones (manual VMM Mem/TLB refresh)** (`RobRunReader.h`, `RobRunReader.cpp`, `PlayerEsp.cpp`)
  - Added lock-free `ReportRenderState(anyOnScreen, nearestOnScreenDist)` from `PlayerEsp::DrawPlayers` so `RobRunReader` triggers `RefreshAllCaches()` (`MEM` + `TLB`) every `2.5 s` when no enemy is rendered on screen, everyone on screen is `> 80 m` away, or no battle root / empty lobby.
  - Added immediate `RefreshAllCaches()` + view/rig cache clear on `> 25 m` local-position jump (full respawn/teleport), location-ID change, and `BattleModel` change (via uncached `ReadFresh` check in `RevalidateSession`), plus clearing `rigCache`/`detailCache` in `MarkSessionChanged`.
  - Added a `600 ms` bone-starvation recovery trigger (`RefreshAllCaches` + fresh view/rig rebuild) and sped up world-worker `RefreshTlbCachePartial` cadence (every 4 ticks quiet / 10 ticks close combat instead of 20).
- **2026-11-21 — Missing legs / torso-only skeletons fixed** (`SkeletonExtractor.cpp`, `RobRunReader.cpp`, `PlayerEsp.cpp`)
  - Fixed `BuildLayoutFromView` fallback stopping 1 hop above `M_spine_2_jnt` (missed `L/R_leg_1_jnt` attaching 2+ hops up, especially on female rigs), fixed `&&` -> `||` single-limb checks, and switched chain measurements to world-space FK.
  - Wired `ReadNamedBones` + `FillMissingAnchorsFromNames` (RobRun `*_jnt` + Mixamo names) before segment finalization in both `BuildLayoutFromView` and `BuildLayoutByName`, reusing the name list for `out.boneMap`.
  - Fixed `EnsureNameOffsetsProbed` to probe `0x30` first across multiple bones with hint scoring and retry on failure.
  - Added immediate `DmaReadFresh` retry on initial builds missing limbs + silent `350 ms` `DmaReadFresh` upgrade retries in `RobRunReader.cpp` (eliminating the 1–3 s `-norefresh` stale-cache delay before legs pop in), plus `poseHistCount` reset on every layout rebuild/upgrade.
  - Removed the `d >= 0.01f` floor in `PlayerEsp.cpp` `IsValidBone` and widened the sanity radius to `3.0 m`.
- **2026-11-21 — Pose jitter, stray limbs, grey-strobing, FOV drift, latency** (`Overlay.cpp`, `CameraFinder.cpp`, `RobRunCamera.cpp`, `RobRunReader.cpp`, `SkeletonExtractor.cpp`, `PlayerEsp.cpp`)
  - Flip-discard swapchain + tearing + 1-frame latency limit; input polling off render thread; screen-space culling.
  - Lock-free seqlock camera snapshots; chunked scatter reads with `SwitchToThread()` yields; `-norefresh` + physical camera reads.
  - Periodic native/Cinemachine FOV refresh for dynamic FOV states.
  - Fixed FK parent-scale bug (`worldScale[p]`); restricted leaf extension to foot toes; replaced hold-gate with pelvis-relative 3-sample temporal median + incomplete-pose rejection.
  - Gated `HiddenAlpha` dimming behind `VisCheck || WallCheck`.

---

## Key Invariants (Do Not Break)

1. **Never keep a moved-from `rh.parent`:** After `out.parent = std::move(parent)`, use `out.parent` (never `rh.isAncestor`).
2. **Reset `poseHistCount = 0; poseHistCursor = 0;` on any layout change:** Otherwise the 3-sample median mixes segment indices from old and new layouts.
3. **Never extend hand leaves:** Hand transforms own weapon/magazine joints (`Weapone1_jnt`, `Magazine1_jnt`). Leaf extension must stay foot-only.
4. **Never drop a valid torso layout during an upgrade retry:** Only replace `rig.layout` when the candidate's `limbScore` strictly beats the current one.
5. **Preserve `SwitchToThread()` chunking in entity/detail scatters:** Unchunked full-lobby scatters starve the camera thread on the FPGA bus.

---

## Verified 2nd-PC Baseline (2026-11-21, ~28-player lobby)

- **Camera:** `2500–2720 Hz` (`src=managed`, `phys=100%`, `avg=331–363 us`)
- **Entity worker:** `81–90 Hz` (`avg=7.6–8.8 ms`, `max=20–40 ms`, zero `[SlowPass]` spikes; `p1=0.3ms`, `p2=4.8–5.7ms`, `p3=0.1–0.4ms`, `det=0.4ms`, `p5=0.9–1.2ms`)
- **Skeletons:** `28/28` / `29/29` (100% coverage, legs + torso intact)
- **Overlay:** `420–680 fps` (`mode=FLIP+TEAR cap=0`)

## Watchlist (Verify on 2nd PC)

- Confirm legs stay present immediately (no 1–3 s pop-in) across unarmed/holster, reload, crouch, sprint, female/male rigs, and vehicle enter/exit transitions.
- Confirm players' bones appear immediately after leaving a map, loading a new map, or doing a full respawn/teleport, with zero combat stutter from manual `RefreshAllCaches()` triggers.
- Check `DLOG` output for `NameOffsetProbe`, `RefineBonemapByName`, and `DmaCacheRefresh` (`idleOrOutOfSight`, `respawnTeleport`, `battleModelChanged`, `boneStarvation`).
- Watch `[Perf]` log for `cam` Hz, `phys%`, `worker` Hz, and `overlay` fps under full-lobby load.
