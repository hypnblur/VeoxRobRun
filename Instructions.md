# VeoxDMA - ROB RUN

External DMA overlay for `ROB RUN.exe` (Unity IL2CPP).

## Requirements

- Windows 10/11 x64
- DMA hardware supported by MemProcFS (FTDI FT601 / FPGA)
- Visual Studio with the **Desktop development with C++** workload
- Runtime libraries beside `debug.exe`:
  - `vmm.dll`
  - `leechcore.dll`
  - `FTD3XX.dll`
  - `medius_capi.dll` (only if the Medius input transport is used)

Get `vmm.dll`, `leechcore.dll` and the other MemProcFS files from the
[MemProcFS releases](https://github.com/ufrisk/MemProcFS/releases). Get the
FTDI library from the
[FTD3XX package](https://ftdichip.com/wp-content/uploads/2024/01/FTD3XXLibrary_v1.3.0.9.zip)
(`FTD3XXLibrary/x64/DLL`).

## Build

Open `VeoxDMA.sln` and build `Debug|x64` or `Release|x64`.

The project declares the `v143` toolset. If only a newer toolset is installed,
select it at build time:

```powershell
MSBuild VeoxDMA.sln /p:Configuration=Debug /p:Platform=x64 /p:PlatformToolset=v145
```

The output is `Build\<Configuration>\VeoxDMA\debug.exe`.

## Run

1. Put `debug.exe` in a folder that also contains `vmm.dll`,
   `leechcore.dll`, `FTD3XX.dll` and (optionally) `medius_capi.dll`.
2. Start `ROB RUN.exe`.
3. Run `debug.exe`.

The overlay attaches to `ROB RUN.exe` automatically and retries while the game
is not running. If a required DLL is missing, it shows an error dialog instead
of failing silently.

## Controls

| Key | Action |
| --- | --- |
| `Insert` | Toggle the menu |
| `Delete` | Exit |

## Notes

- All DMA reads run on a dedicated reader thread. The render thread only draws
  the latest published snapshot, so a slow DMA pass does not stall the overlay.
- Offsets are pinned to a specific game build. See
  [docs/ROBRUN_MEMORY.md](docs/ROBRUN_MEMORY.md) and
  `VeoxDMA/SDK/RobRunSDK.h`.
- The overlay is read-only.
