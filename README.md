# VeoxDMA - RobRun

High-performance, multi-threaded external DMA instrumentation suite for `ROB RUN.exe` (Unity IL2CPP x64).

---

## Architecture Overview

VeoxDMA employs a decoupled, multi-worker threading pipeline designed to eliminate hardware transfer latency from rendering and aim calculation paths:

```
┌────────────────────────────────────────────────────────┐
│                   Target Process (DMA)                 │
└───────┬────────────────────────┬───────────────┬───────┘
        │                        │               │
        ▼                        ▼               ▼
┌──────────────────┐  ┌──────────────────┐  ┌──────────────────┐
│  g_workerThread  │  │g_worldWorkerThread│  │g_aimWorkerThread │
│ (Player / Poses) │  │(World/Inventory) │  │(Targeting/Recoil)│
│  Rate: 50–60 Hz  │  │   Rate: 2–5 Hz   │  │   Rate: 500 Hz   │
└────────┬─────────┘  └────────┬─────────┘  └────────┬─────────┘
         │                     │                     │
         └──────────┬──────────┘                     │
                    ▼                                │
      ┌───────────────────────────┐                  │
      │ Lock-Free Snapshot Buffer │                  │
      │ (std::shared_ptr<const>)  │                  │
      └─────────────┬─────────────┘                  │
                    │                                │
                    ▼                                ▼
      ┌───────────────────────────┐    ┌───────────────────────────┐
      │  D3D11 / ImGui Overlay    │    │ Memory / KMBox Controller │
      │   Frame Rate: 144+ FPS    │    │  Microsecond Precision    │
      └───────────────────────────┘    └───────────────────────────┘
```

1. **Player Pose Worker (`g_workerThread`, 50–60 Hz)**:
   - Resolves live `BattleEntitiesModel` entries and extracts bone hierarchies via native `TransformHierarchy` arrays.
   - Scatter-reads character positions and full skeletal matrix buffers in unified hardware transactions.
   - Publishes immutable `Snapshot` instances to eliminate read contention.
   - Adaptive smoothing bypass for moving entities ($> 0.05\text{ m/tick}$) eliminates trailing skeleton lag while preserving jitter suppression on stationary models.

2. **World & Inventory Worker (`g_worldWorkerThread`, 2–5 Hz)**:
   - Traverses complex IL2CPP dictionaries across `WorldEntitiesModel` (loot, containers, workbenches, vehicles, alarms) and local inventory slots asynchronously.
   - Decoupled from player tracking to prevent heavy dictionary scans from causing frame-time spikes.

3. **High-Frequency Aim Worker (`g_aimWorkerThread`, 500 Hz)**:
   - Dedicated $2\text{ ms}$ processing loop executing hardware camera synchronization (`RefreshCamera()`), target tracking, and dynamic recoil/spread compensation.
   - Instant interaction module for zero-delay container looting and item usage.

4. **Overlay Render Thread (144+ FPS)**:
   - Pure GPU projection consuming published snapshots. Zero DMA transactions occur on the render thread.

---

## Key Features

### 1. Player ESP & Skeletal Projection
- **Native Hierarchy Extraction**: Navigates Unity `TransformHierarchy` arrays to dynamically construct 3D bone links rather than assuming fixed joint counts.
- **Occlusion-Aware Color Cues**: Vis-checked bone segments dynamically switch between visible and occluded colors via real-time BVH raycasting.
- **Dynamic Bounding Boxes**: Tight AABB corner boxes calculated from projected bone extents with fallback to character root bounds.
- **Comprehensive Vitals & State Indicators**:
  - Exact numerical health, max health, and scaled health bars.
  - Armor presence and numerical durability values.
  - Stamina meter percentages.
  - Active animation flags: firing, sprinting, crouching, reloading (with descriptive phase text), knocked down, knocked out, surrendered, and handcuffed.
  - Target orientation indicator ("Looking at you" warning).
  - Spectator / observer count alerts with on-screen notification banners.
- **Weapon Icons**: Procedurally rendered weapon silhouettes and active loadout names.

### 2. World ESP & Table Traversal
- **Money Workbenches**: Full two-level description resolution with real-time extraction of clean cash, dirty cash, and combined total stack values (`$Total [C:$Clean D:$Dirty]`).
- **Collection Tables**: Deep reward model inspection displaying collectable cash sums.
- **Containers & Safe Doors**: Containers with live empty vs. loot status indicators, safe doors, storage boxes, shelvings, and plot containers.
- **Cash & Valuables**: Settlement cash points, cash loot nodes, crypto workbenches, and loose currency.
- **Vehicles & Heavy Equipment**: Cars, helicopters, vehicle loot, and vehicle trunks.
- **Security & Medical**: Security alarms, location alarms, and first aid cabinets.
- **Points of Interest & Cargo**: Dropped loot bags, cargo drops, and map markers.
- **Instant Looting**: Background write acceleration that instantly completes `ProlongedInteractionDialog` progress and collapses `DelayedUseAction` interaction delays.

### 3. BVH Hardware-Accelerated Line-of-Sight
- **Surface Area Heuristic (SAH) BVH**: Loads binary bounding volume hierarchies (`city_colliders.bvh`) containing tens of thousands of scene triangles.
- **Depth-Optimized DFS Traversal**: 128-node stack traversal supporting deep collider hierarchies without heap allocations.
- **Near-Plane Lens Clipping Rejection**: Near-plane guard ($t_{\min} = 0.20\text{ m}$) prevents camera lens grazing from triggering false-positive whole-screen occlusion.
- **Per-Bone Verification**: Target validation checks head, neck, chest, and pelvis individually before dropping targets behind cover.

### 4. Targeting & Recoil Compensation
- **Memory & Hardware Aim Support**: Direct memory angle manipulation or hardware cursor emulation via KMBox (B+ Pro / Net).
- **Dynamic Recoil & Spread Neutralization**: Continuously writes recoil pitch/yaw offsets to zero in sync with weapon firing cycles.
- **Dynamic Bone Priority Fallback**: Automatically cascades through candidate target bones (`Head` $\to$ `Neck` $\to$ `Chest` $\to$ `Pelvis`) when upper bones become occluded.
- **Party Member Filtering**: Resolves player account public IDs and party models from `BattleEntitiesModel` to exempt teammates from targeting regardless of in-game role assignments.

### 5. Radar, UI & Config Management
- **Multi-Map Radar**: Top-down radar supporting 6 calibrated map layouts (Looty Bay, Hangar, Lenny's Motel, Prison, Settlement, Settlement 2) with auto-scaling and orientation alignment.
- **Modular JSON Configs**: Independent serialization and hot-reloading for Player, World, Aim, Radar, and Overlay settings.
- **DirectX 11 ImGui Interface**: Clean, dark-themed menu with customizable hotkeys, color pickers, and live performance metrics.

---

## Directory Structure

```
VeoxDMA/
├── Aim/                 # Targeting engine, recoil compensation, KMBox drivers
├── Config/              # Configuration schemas (Aim, Player, Radar, World, Overlay)
├── Esp/                 # D3D11 visual renderers (PlayerEsp, WorldEsp, Radar, WeaponSprites)
├── ImGUI/               # ImGui backend and rendering primitives
├── Include/             # LeechCore, VMMDLL, and FPGA headers
├── Input/               # KMBox B+ Pro and KMBox Net hardware interfaces
├── Lib/                 # Precompiled FPGA / LeechCore / VMM libraries
├── Memory/              # Core DMA engine, physical memory scatter/gather APIs
├── Misc/                # Math utilities, Vector2, Vector3, Matrix4x4
├── Overlay/             # Win32 transparent click-through window, D3D11 swapchain
├── SDK/                 # ROB RUN IL2CPP schemas, camera matrices, BVH collision, readers
│   ├── CollisionBVH.h       # SAH BVH tree parser & ray-triangle intersection
│   ├── Il2Cpp.h             # IL2CPP runtime type & metadata traversal
│   ├── RobRunCamera.h/cpp   # View-projection construction & FOV math
│   ├── RobRunMemory.h/cpp   # DMA read/write primitives & dictionary parsers
│   ├── RobRunReader.h/cpp   # Master worker thread, snapshot publisher, camera sync
│   ├── RobRunSDK.h          # Verified IL2CPP class offsets & struct definitions
│   └── WorldEntities.h/cpp  # World loot, table traversal, container parsing
└── UI/                  # VeoxMenu interactive settings interface
OrbitResources/         # Resource-only DLL carrying the loot filter icon set
tools/
└── loot/               # Loot catalog and icon generators (see tools/loot/README.md)
```

---

## Build Instructions

### Prerequisites
- Windows 10 / 11 (x64)
- Visual Studio 2022 (Community, Professional, or Enterprise)
- C++20 Standard Support (`/std:c++20`)
- MSVC v143 or v145 Platform Toolset

### Building via Visual Studio / MSBuild
Run from the Developer Command Prompt for VS 2022:

```powershell
# Build Release x64 with MSVC v145 toolset
MSBuild.exe VeoxDMA.sln /p:Configuration=Release /p:Platform=x64 /p:PlatformToolset=v145 /m
```

The compiled binary will be placed in:
`Build/Release/VeoxDMA/VeoxDMA.exe`

### Runtime Dependencies
Ensure the following files reside in the working directory next to `VeoxDMA.exe`:
- `vmm.dll` & `leechcore.dll` (PCILeech DMA libraries)
- `FTD3XX.dll` (FTDI SuperSpeed USB driver)
- `OrbitResources.dll` (Loot filter icons, committed in `Lib/`; the build copies it next to the exe. To regenerate it see [Updating OrbitResources.dll](tools/loot/README.md#updating-orbitresourcesdll))
- `city_colliders.bvh` (Map collider geometry for line-of-sight verification)
- `maps/` (Radar minimap textures: `looty_bay.png`, `hangar.png`, etc.)

---

## Reference Documentation
- [docs/ROBRUN_MEMORY.md](docs/ROBRUN_MEMORY.md): In-depth memory offset map, IL2CPP object layouts, and class inspection records.
- [docs/AIM.md](docs/AIM.md): Target prioritization math, coordinate projection, and recoil compensation internals.
- [docs/PROJECTION_LATENCY.md](docs/PROJECTION_LATENCY.md): Comprehensive projection timing, latency budget, and synchronization analysis.

---

## Credits
- [PCILeech](https://github.com/ufrisk/pcileech)
- [MemProcFS](https://github.com/ufrisk/MemProcFS)
- [IL2CPP Dumper](https://github.com/Perfare/Il2CppDumper)
- [ImGui](https://github.com/ocornut/imgui)
