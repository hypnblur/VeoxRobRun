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
* D3D11 overlay with transparency, click-through, always-on-top, frame cap and notifications

## Memory reference
The build-pinned offset map and implementation notes are in
[docs/ROBRUN_MEMORY.md](docs/ROBRUN_MEMORY.md). Canonical constants live in
`VeoxDMA/SDK/RobRunSDK.h`.

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
