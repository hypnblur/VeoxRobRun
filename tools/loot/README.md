# Loot tooling

Scripts that build the loot filter catalog (`VeoxDMA/Config/LootFilterData.h`)
and the icon DLL (`OrbitResources.dll`). Run everything from the repo root.

The catalog and a built `Lib/OrbitResources.dll` are committed, so a normal
build needs none of this. Re-run it after a game update that adds or renames
items.

## Updating OrbitResources.dll

The overlay loads `Lib/OrbitResources.dll`, which is committed; the VeoxDMA
post-build step copies it next to `VeoxDMA.exe`. The `OrbitResources` project
is not part of the normal solution build, because it embeds
`OrbitResources/icons/*.png`, which is git-ignored and has to be extracted
from the game first. To refresh the DLL:

Requirements: Python 3, Pillow (`pip install pillow`), and Rob Run installed
and **closed**. The first run downloads AssetStudioModCLI (pinned and SHA-256
checked) into `tools/loot/.asmod/`.

1. Extract the icons from the game install:

   ```powershell
   python tools/loot/update_icons.py --game "path\to\Rob Run"
   ```

   This only reads the game's files on disk. It exports every sprite to
   `tools/loot/icons/` and rewrites `tools/loot/data/icon_names.hpp`. Without
   `--game` it asks for the folder.

2. Lay out the icon set for the DLL:

   ```powershell
   python tools/loot/gen_orbit_icons.py
   ```

   Copies every icon the catalog uses into `OrbitResources/icons/` and rewrites
   `OrbitResources/icons.rc` and `OrbitResources/resource.h`. Pass `--size 64`
   to downscale to square icons for a smaller DLL.

3. Build the `OrbitResources` project for `Release|x64` (right-click it in
   Visual Studio > Build, or
   `MSBuild OrbitResources/OrbitResources.vcxproj /p:Configuration=Release /p:Platform=x64 /p:SolutionDir=%CD%\`).
   It copies the result over `Lib/OrbitResources.dll`; commit that file.

   The Release DLL is built without debug info and with `/Brepro`, so it holds
   no PDB path, timestamp or toolchain stamp, and the same inputs give the same
   bytes.

If the game update changed the icon list, also copy
`tools/loot/data/icon_names.hpp` over `VeoxDMA/Esp/IconIndex.h` and re-run
`gen_loot_catalog.ps1` (step 4 below) before step 2.

## Layout

| Path | Committed | Contents |
|---|---|---|
| `data/item-database.tsv` | yes | id, name, category, rarity, price per item |
| `data/loot-registry.tsv` | yes | ids and entity kinds from the game registries |
| `data/icon_names.hpp` | yes | sprite to item id to English name index |
| `data/item-prices.tsv` | yes | optional price overrides |
| `icons/` | no | sprites exported from the game install |
| `.asmod/` | no | AssetStudioModCLI, fetched on first run |
| `work/` | no | JSON dumps, IL2CPP dump, reports |

## Pipeline

1. `python tools/loot/update_icons.py --game "path\to\Rob Run"`
   With the game closed, exports every sprite to `icons/` and writes
   `data/icon_names.hpp`. It only reads files on disk.
2. `python tools/loot/gen_loot_registries.py`
   Reads the registry assets dumped under `work/` and writes `data/loot-registry.tsv`.
3. `python tools/loot/parse_item_database.py "path\to\saved item database.html"`
   Writes `data/item-database.tsv`. This is the only source of item prices.
4. `powershell -File tools/loot/gen_loot_catalog.ps1`
   Joins the tables into `VeoxDMA/Config/LootFilterData.h`.
5. `python tools/loot/gen_orbit_icons.py`
   Lays out the icon set for `OrbitResources.dll` (see above), then build.

Audit: `python tools/loot/check_icons.py` lists catalog rows whose sprite is
not embedded in `OrbitResources/icons.rc`, and staged icons no row uses.

Optional price research: `extract_prices.py` (shipped data) and `mine_prices.py`
(IL2CPP dump in `work/il2cpp-dump`).
