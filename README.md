# VeoxDMA - RobRun

External DMA overlay for `ROB RUN.exe` (Unity IL2CPP).

## Features
* Player ESP: skeleton, corner box, nickname, clan tag, distance, health bar
* World ESP: containers (with empty state), loot, vehicles, cars, helicopters, grenades
* Camera projection from `BattleCameraModel` plus Cinemachine vertical FOV
* Local inventory reader: packed slot decode, grid size, backpack lines
* Optional item catalog reader (bounded, opt-in class scan)
* IL2CPP metadata/field resolver with class and backing-field validation
* Build fingerprint and accessor-byte checks with periodic session revalidation
* Reader worker thread publishes an immutable snapshot; the overlay thread never performs DMA reads
* Map radar with multi-map support (Looty Bay, Hangar, Lenny's Motel, Prison, Settlement, Settlement 2)
* Fullscreen backdrop window with custom resolution support
* D3D11 overlay with transparency, click-through, always-on-top, frame cap and notifications

## Memory reference
The build-pinned offset map and implementation notes are in
[docs/ROBRUN_MEMORY.md](docs/ROBRUN_MEMORY.md). Canonical constants live in
`VeoxDMA/SDK/RobRunSDK.h`.

## Skeleton notes
Read each character's own rendered transforms and follow their actual native
parent hierarchy rather than relying on a fixed bone count at `hierarchy +0x38`
(it can change while the rig is still valid). Validate joints and connections
individually so one bad limb doesn't drop the whole skeleton. Sampling poses
separately from the render loop and interpolating recent valid samples reduces
jitter.

For skeletons disappearing after world changes: the updater can become null and
the view registry empties while old model pointers keep the resolver marked as
ready. Check the current active scene's updater, clear old model/view/rig/transform
caches when it changes, then resolve and bind again. Retry when the updater or
registry is missing even if model pointers still pass type checks.

## Build
Open `VeoxDMA.sln` and build `Debug|x64` (or `Release|x64`). The project
declares `v143`; newer toolchains can be selected at build time, for example:

```powershell
MSBuild VeoxDMA.sln /p:Configuration=Debug /p:Platform=x64 /p:PlatformToolset=v145
```

See [Instructions.md](Instructions.md) for the DMA runtime dependencies.

## Credits
* [PCILeech](https://github.com/ufrisk/pcileech)
* [MemProcFS](https://github.com/ufrisk/MemProcFS)
* [IL2CPP Dumper](https://github.com/Perfare/Il2CppDumper)
* [DMALibrary](https://github.com/Metick/DMALibrary/tree/Master)
