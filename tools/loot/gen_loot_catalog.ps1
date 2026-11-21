<#
.SYNOPSIS
    Regenerate VeoxDMA/Config/LootFilterData.h.

.DESCRIPTION
    Two sources, in this order:

      1. data/item-database.tsv  (parse_item_database.py)
         The wiki item database: 563 items with the game id, display name,
         one of 23 categories, rarity and market price. Category, name, rarity
         and price for any item listed here come from this file - it is the only
         price source the project has, because the shipped game data carries no
         numeric item prices.

      2. data/loot-registry.tsv  (gen_loot_registries.py)
         The game's own registry assets, which say which ids exist and which
         entity kind they belong to. Anything the database does not list keeps a
         category derived from its registry.

    Entity kinds the reader can distinguish but the game has no registry for
    (cash nodes, workbenches, alarms, containers, ...) get one row each.
#>
[CmdletBinding()]
param(
    [string]$Registry,
    [string]$Database,
    [string]$OutFile,
    [string]$PricesFile,
    [string]$IconIndex
)

$ErrorActionPreference = 'Stop'
$here = Split-Path -Parent $PSCommandPath
$repo = Split-Path -Parent (Split-Path -Parent $here)
if (-not $Registry)   { $Registry   = Join-Path $here 'data\loot-registry.tsv' }
if (-not $Database)   { $Database   = Join-Path $here 'data\item-database.tsv' }
if (-not $OutFile)    { $OutFile    = Join-Path $repo 'VeoxDMA\Config\LootFilterData.h' }
if (-not $PricesFile) { $PricesFile = Join-Path $here 'data\item-prices.tsv' }
if (-not $IconIndex)  { $IconIndex  = Join-Path $here 'data\icon_names.hpp' }

# ---------------------------------------------------------------- helpers
function CStr([string]$s) {
    $sb = New-Object System.Text.StringBuilder
    [void]$sb.Append('"')
    foreach ($b in [System.Text.Encoding]::UTF8.GetBytes($s)) {
        $c = [char]$b
        if ($b -eq 0x22) { [void]$sb.Append('\"') }
        elseif ($b -eq 0x5C) { [void]$sb.Append('\\') }
        elseif ($b -ge 0x20 -and $b -lt 0x7F) { [void]$sb.Append($c) }
        else { [void]$sb.Append(('\{0:000}' -f $b)) }
    }
    [void]$sb.Append('"')
    return $sb.ToString()
}

function TitleCase-Id([string]$id) {
    $parts = $id -split '_' | Where-Object { $_ }
    ($parts | ForEach-Object { $_.Substring(0,1).ToUpper() + $_.Substring(1) }) -join ' '
}

# Ids are spelled differently by different parts of the game: placement ids use
# "vest_4_mk_1" while item ids use "vest4_m1", and pickups are sometimes just
# "lmg". Folding separators and "mk" -> "m" lets those meet in the middle.
function Norm([string]$value) {
    $flat = $value.ToLowerInvariant() -replace '[^a-z0-9]', ''
    return ($flat -replace 'mk', 'm')
}

function EnumId([string]$label) {
    ($label -split '[^A-Za-z0-9]' | Where-Object { $_ } |
        ForEach-Object { $_.Substring(0,1).ToUpper() + $_.Substring(1) }) -join ''
}

# ---------------------------------------------------------------- categories
# The wiki's 23 categories, then the three the world reader needs for entities
# the database does not list.
$categoryLabels = @(
    'Ammo', 'Armor', 'Breaching tools', 'Car upgrades', 'Cargo', 'Containers',
    'Cumulative items', 'Documents', 'Furniture & objects', 'Grenades', 'Home & plot',
    'Keys', 'Laboratory', 'Loot Nodes', 'Medicine', 'Misc', 'NPC', 'Plot buildings',
    'Security', 'Tools', 'Valuables', 'Vehicles', 'Weapon maintenance', 'Weapons',
    'Workbenches & upgrades', 'World Loot'
)
$categoryDefaultRarity = @{
    'Containers' = 'Common'; 'Loot Nodes' = 'Common'; 'World Loot' = 'Common'
    'Valuables' = 'Rare'; 'Cargo' = 'Uncommon'; 'Vehicles' = 'Uncommon'
    'Weapons' = 'Uncommon'; 'Armor' = 'Uncommon'; 'Ammo' = 'Common'; 'Medicine' = 'Common'
    'Grenades' = 'Uncommon'; 'Security' = 'Uncommon'; 'Laboratory' = 'Common'
    'Misc' = 'Common'; 'Tools' = 'Common'; 'Keys' = 'Uncommon'; 'Documents' = 'Common'
}
$categoryColour = @{
    'Containers' = @(255,170,60); 'Loot Nodes' = @(255,190,80); 'World Loot' = @(120,255,140)
    'Valuables' = @(255,215,0); 'Cargo' = @(255,230,120); 'Weapons' = @(255,90,90)
    'Ammo' = @(210,190,140); 'Armor' = @(120,190,255); 'Medicine' = @(120,255,200)
    'Laboratory' = @(0,220,100); 'Grenades' = @(255,120,60); 'Vehicles' = @(120,190,255)
    'Security' = @(255,75,75); 'Tools' = @(200,200,210); 'Keys' = @(255,200,120)
    'Documents' = @(200,205,215); 'Car upgrades' = @(150,200,255)
    'Workbenches & upgrades' = @(200,180,255); 'Home & plot' = @(180,220,160)
    'Furniture & objects' = @(200,180,150); 'Plot buildings' = @(170,200,200)
    'Cumulative items' = @(220,200,255); 'NPC' = @(255,160,200)
    'Breaching tools' = @(255,140,120); 'Weapon maintenance' = @(230,180,140)
    'Misc' = @(200,205,215)
}
$categoryId = @{}
foreach ($label in $categoryLabels) { $categoryId[$label] = (EnumId $label) }

# ---------------------------------------------------------------- inputs
$knownSprites = New-Object System.Collections.Generic.HashSet[string]
$carBaseById = @{}
$rx = [regex]'\{"((?:[^"\\]|\\.)*)", "([^"]*)", "((?:[^"\\]|\\.)*)", "((?:[^"\\]|\\.)*)", "((?:[^"\\]|\\.)*)"'
foreach ($line in [System.IO.File]::ReadLines($IconIndex)) {
    $m = $rx.Match($line)
    if (-not $m.Success) { continue }
    if ($m.Groups[2].Value) { [void]$knownSprites.Add($m.Groups[1].Value) }
    $id = $m.Groups[3].Value
    $base = $m.Groups[5].Value
    if ($id -and $base -and -not $carBaseById.ContainsKey($id.ToLowerInvariant())) {
        $carBaseById[$id.ToLowerInvariant()] = $base
    }
}

# Wiki database: id -> name / category / rarity / price.
$db = @{}
$dbByName = @{}
$dbCategories = New-Object System.Collections.Generic.HashSet[string]
if (Test-Path $Database) {
    foreach ($line in [System.IO.File]::ReadLines($Database)) {
        if (-not $line -or $line.StartsWith('#')) { continue }
        $p = $line -split "`t"
        if ($p.Count -lt 5) { continue }
        $entry = @{ Name = $p[1]; Category = $p[2]; Rarity = $p[3]; Price = [int]$p[4] }
        $db[$p[0].ToLowerInvariant()] = $entry
        $nameKey = ($p[1].ToLowerInvariant() -replace '[^a-z0-9]', '')
        if ($nameKey -and -not $dbByName.ContainsKey($nameKey)) { $dbByName[$nameKey] = $entry }
        if ($p[2]) { [void]$dbCategories.Add($p[2]) }
    }
}
foreach ($c in $dbCategories) {
    if (-not $categoryId.ContainsKey($c)) {
        throw "database category '$c' is not in the category list - add it to `$categoryLabels"
    }
}
Write-Host ("database  {0} items, {1} categories" -f $db.Count, $dbCategories.Count)

# Manual overrides: id<TAB>value[<TAB>rarity], wins over the database.
$prices = @{}
$rarityOverride = @{}
if (Test-Path $PricesFile) {
    foreach ($line in [System.IO.File]::ReadLines($PricesFile)) {
        if (-not $line -or $line.StartsWith('#')) { continue }
        $parts = $line -split "`t"
        if ($parts.Count -lt 2) { continue }
        $key = $parts[0].Trim().ToLowerInvariant()
        $value = 0
        if ([int]::TryParse($parts[1].Trim(), [ref]$value)) { $prices[$key] = $value }
        if ($parts.Count -ge 3) {
            $r = $parts[2].Trim()
            if ($r -in @('Legendary','Epic','Rare','Uncommon','Common','Exotic')) { $rarityOverride[$key] = $r }
        }
    }
}

$registryMap = @{
    'cargo'      = @{ Cat = 'Cargo';      Kind = 'Cargo' }
    'item'       = @{ Cat = 'Misc';       Kind = 'Loot' }
    'lootobject' = @{ Cat = 'World Loot'; Kind = 'StaticLoot' }
    'lootnode'   = @{ Cat = 'Loot Nodes'; Kind = 'LootNode' }
    'container'  = @{ Cat = 'Containers'; Kind = 'Container' }
    'car'        = @{ Cat = 'Vehicles';   Kind = 'Car' }
}

$catalog = New-Object System.Collections.Generic.List[object]
$seen = New-Object System.Collections.Generic.HashSet[string]

function Add-Row([string]$Cat, [string]$Id, [string]$Name, [string]$Sprite, [string]$Rarity,
                 [string]$Kind, [string]$Match) {
    $key = "$Cat|$Id"
    if (-not $seen.Add($key)) { return }
    # Fold the sprite name exactly as the icon pipeline does (non-safe to '_'),
    # so the catalog string and the rc resource name are the same identifier.
    if ($Sprite) {
        $folded = -join ($Sprite.ToCharArray() | ForEach-Object {
            if ($_ -match '[A-Za-z0-9_]') { $_ } else { '_' }
        })
        $Sprite = $folded
    }
    if ($Sprite -and -not $knownSprites.Contains($Sprite)) { $Sprite = '' }
    $catalog.Add([pscustomobject]@{
        Cat = $Cat; Id = $Id; Name = $Name; Sprite = $Sprite; Rarity = $Rarity; Kind = $Kind; Match = $Match
    })
}

# --- entity kinds with no game registry -----------------------------------
$kinds = @(
    # Cargo whose type could not be read is the spawn node itself, not a cargo
    # item, so it lives with the loot nodes rather than in the Cargo category.
    @('Loot Nodes',         'cargo',                 'Cargo',                 '', 'Common',   'Cargo',              ''),
    @('Valuables',          'cash_loot_node',        'Cash Loot Node',        '', 'Common',   'CashLootNode',       ''),
    @('Valuables',          'cash_settlement',       'Settlement Cash',       '', 'Uncommon', 'SettlementCash',     ''),
    @('Valuables',          'cash_money_workbench',  'Money Workbench',       '', 'Rare',     'MoneyWorkbench',     ''),
    @('Valuables',          'cash_collection_table', 'Collection Table',      '', 'Rare',     'CollectionTable',    ''),
    @('Valuables',          'cash_dropped_bag',      'Dropped Bag',           '', 'Common',   'DroppedBag',         ''),
    @('Valuables',          'cash_crypto_workbench', 'Crypto Workbench',      '', 'Epic',     'CryptoWorkbench',    ''),
    @('Containers',         'cont_safe_door',        'Safe Door',             '', 'Epic',     'SafeDoor',           ''),
    @('Containers',         'cont_storage_box',      'Storage Box',           '', 'Common',   'StorageBox',         ''),
    @('Containers',         'cont_shelving',         'Shelving',              '', 'Common',   'Shelving',           ''),
    @('Containers',         'cont_plot_container',   'Plot Container',        '', 'Common',   'PlotContainer',      ''),
    @('Containers',         'cont_vehicle_trunk',    'Vehicle Trunk',         '', 'Uncommon', 'VehicleTrunk',       ''),
    @('Containers',         'cont_first_aid',        'First Aid Cabinet',     '', 'Uncommon', 'FirstAidCabinet',    ''),
    @('Loot Nodes',         'loot_node',             'Loot Node',             '', 'Common',   'LootNode',           ''),
    @('Security',           'sec_alarm',             'Security Alarm',        '', 'Common',   'SecurityAlarm',      ''),
    @('Security',           'sec_location_alarm',    'Location Security Alarm','', 'Common',  'LocationSecurityAlarm', ''),
    @('Security',           'sec_defense_block',     'Defense Block',         '', 'Uncommon', 'DefenseBlock',       ''),
    @('Laboratory',         'drug_workbench',        'Drug Workbench',        '', 'Rare',     'DrugWorkbench',      ''),
    @('World Loot',         'world_random_loot',     'Random Loot',           '', 'Common',   'RandomLoot',         ''),
    @('World Loot',         'world_loot',            'Loot',                  '', 'Common',   'Loot',               ''),
    @('World Loot',         'world_vehicle_loot',    'Vehicle Loot',          '', 'Common',   'VehicleLoot',        ''),
    @('World Loot',         'world_plot_loot',       'Plot Loot',             '', 'Common',   'PlotLoot',           ''),
    @('World Loot',         'world_furniture_loot',  'Furniture Loot',        '', 'Common',   'FurnitureLoot',      ''),
    @('World Loot',         'world_building_loot',   'Building Loot',         '', 'Common',   'BuildingLoot',       ''),
    @('Vehicles',           'veh_helicopter',        'Romanov R8',            'romanov_r8', 'Rare', 'Helicopter',      ''),
    @('Grenades',           'gren_grenade',          'Grenade',               '', 'Common',   'Grenade',            '')
)
foreach ($k in $kinds) { Add-Row $k[0] $k[1] $k[2] $k[3] $k[4] $k[5] $k[6] }

# --- registry rows --------------------------------------------------------
if (-not (Test-Path $Registry)) {
    throw "registry table not found: $Registry (run gen_loot_registries.py first)"
}
$cargoIds = New-Object System.Collections.Generic.HashSet[string]
foreach ($line in [System.IO.File]::ReadLines($Registry)) {
    if (-not $line -or $line.StartsWith('#')) { continue }
    $p = $line -split "`t"
    if ($p.Count -lt 4 -or $p[0] -ne 'cargo') { continue }
    [void]$cargoIds.Add($p[1].ToLowerInvariant())
}

$seenIds = New-Object System.Collections.Generic.HashSet[string]
$itemByNorm = @{}
foreach ($line in [System.IO.File]::ReadLines($Registry)) {
    if (-not $line -or $line.StartsWith('#')) { continue }
    $p = $line -split "`t"
    if ($p.Count -lt 4 -or $p[0] -ne 'item' -or -not $p[3]) { continue }
    $key = Norm $p[1]
    if ($key -and -not $itemByNorm.ContainsKey($key)) {
        $itemByNorm[$key] = @{ Name = $p[2]; Sprite = $p[3] }
    }
}

$registryRows = 0
foreach ($line in [System.IO.File]::ReadLines($Registry)) {
    if (-not $line -or $line.StartsWith('#')) { continue }
    $p = $line -split "`t"
    if ($p.Count -lt 4) { continue }
    $reg, $id, $name, $sprite = $p[0], $p[1], $p[2], $p[3]
    $map = $registryMap[$reg]
    if (-not $map) { continue }

    if ($reg -eq 'item' -and $cargoIds.Contains($id.ToLowerInvariant())) { continue }
    if (-not $seenIds.Add($id.ToLowerInvariant())) { continue }

    # Placement rows are kept even when an item row normalises to the same id.
    # The lookup is kind-sensitive, so a world entity labelled "ap_scalar_m_1"
    # could not borrow the inventory item's row and fell through to the generic
    # catch-all - which is why weapons disappeared while bottles did not.
    # Both rows exist; the family pass below gives the placement row the same
    # name, art and rarity as its item.

    if ($reg -eq 'car') {
        $baseName = $carBaseById[$id.ToLowerInvariant()]
        if ($baseName) { $name = $baseName }
    }

    if (-not $name) { $name = TitleCase-Id $id }
    Add-Row $map.Cat $id $name $sprite $categoryDefaultRarity[$map.Cat] $map.Kind $id
    $registryRows++
}

# --- database pass: category, name, rarity and price from the wiki ---------
# The wiki keys for these two bottles are reversed relative to the shipped registry.
$wikiIdByRegistryId = @{
    'alcohol_2' = 'alcohol_4'
    'alcohol_4' = 'alcohol_2'
}
foreach ($row in $catalog) {
    # Generic entity rows (empty label match) are buckets for entities the reader
    # can only describe by kind. They are not items, so the database must not
    # re-categorise them - that is how the cargo spawn row kept landing back in
    # the Cargo category through the item named "Cargo".
    if (-not $row.Match) {
        $row | Add-Member -NotePropertyName DbPrice -NotePropertyValue 0 -Force
        continue
    }
    $dbId = $row.Id.ToLowerInvariant()
    if ($wikiIdByRegistryId.ContainsKey($dbId)) {
        $dbId = $wikiIdByRegistryId[$dbId]
    }
    $hit = $db[$dbId]
    if (-not $hit) {
        # Registries and the wiki spell some ids differently
        # ("vulture50_m1" vs "vulture_m1"), so compare them with separators and
        # digits intact first, then fall back to the display name.
        $norm = Norm $row.Id
        $match = $db.Keys | Where-Object { (Norm $_) -eq $norm } | Select-Object -First 1
        if ($match) { $hit = $db[$match] }
    }
    if (-not $hit -and $row.Name) {
        $nameKey = ($row.Name.ToLowerInvariant() -replace '[^a-z0-9]', '')
        if ($nameKey -and $dbByName.ContainsKey($nameKey)) { $hit = $dbByName[$nameKey] }
    }
    if ($hit) {
        if ($hit.Name) { $row.Name = $hit.Name }
        if ($hit.Category) { $row.Cat = $hit.Category }
        if ($hit.Rarity) { $row.Rarity = $hit.Rarity }
        $row | Add-Member -NotePropertyName DbPrice -NotePropertyValue $hit.Price -Force
    } else {
        $row | Add-Member -NotePropertyName DbPrice -NotePropertyValue 0 -Force
    }
}

# --- placeholder pickups ---------------------------------------------------
# A world loot object with no display name, no icon and no database entry is a
# bare placement id ("lmg", "star_h_m_2", "grenade_0"). Before dropping it, look
# for an item family with the same prefix: "lmg" covers lmg_m1/lmg_m2, so the
# pickup can borrow the family name and art instead of vanishing from the list.
$before = $catalog.Count
$kept = New-Object System.Collections.Generic.List[object]
$dropped = 0
foreach ($row in $catalog) {
    # A generic silhouette ("Icon_WeaponStarH") is worse than the family's real
    # art, so treat it exactly like a missing sprite.
    $genericArt = $row.Sprite -like 'Icon_*'
    # Sprite-driven: any row without real art borrows the family's. The name is
    # irrelevant here - the database pass may already have given it the item name,
    # which is exactly the case that kept the generic silhouette.
    $placeholder = ((-not $row.Sprite) -or $genericArt)
    if (-not $placeholder) { $kept.Add($row); continue }

    # Family key: the id with its tier marks stripped, so "ap_scalar_1" and
    # "apscalar_m1" both reduce to "apscalar" and can be matched to each other.
    function FamilyKey([string]$value) {
        $flat = Norm $value
        return ($flat -replace '(m)?\d+$', '')
    }
    $flat = FamilyKey $row.Id
    $family = $null
    if ($flat) {
        # The tier digit in the placement id selects the matching item: the world
        # object "star_h_m_2" is the Mk2, so borrowing Mk1's art and name would be
        # wrong by one tier.
        $tier = ''
        if ((Norm $row.Id) -match '(\d+)$') { $tier = $Matches[1] }
        $anyFamily = $null
        foreach ($candidate in $catalog) {
            if ($candidate.Id -eq $row.Id) { continue }
            if (-not $candidate.Sprite) { continue }
            if ((FamilyKey $candidate.Id) -ne $flat) { continue }
            if (-not $anyFamily) { $anyFamily = $candidate }
            $candidateFlat = Norm $candidate.Id
            if ($tier -and ($candidateFlat.EndsWith("m$tier") -or $candidateFlat.EndsWith($tier))) {
                $family = $candidate
                break
            }
        }
        if (-not $family) { $family = $anyFamily }
    }
    if (-not $family) {
        if ($genericArt) {
            $row.Sprite = ''
        }
        $kept.Add($row)
        continue
    }

    # Keep the family name as-is: it already carries the tier, and stripping it
    # turned "STAR-H Mk2" into "STAR-H".
    $row.Name = $family.Name
    $row.Sprite = $family.Sprite
    $row.Rarity = $family.Rarity
    $kept.Add($row)
}
$catalog = $kept
Write-Host ("placeholders {0} kept as family rows, {1} dropped" -f ($before - $dropped - $kept.Count + ($before - $kept.Count - $dropped) * 0), $dropped)

# --- placement -> item aliases --------------------------------------------
# A placement id that is only the pickup for an item the catalog already knows:
# borrow that item's name, art, rarity and price so the row is not a bare mesh
# name. Anything not listed here still gets the family name from the pass above.
$aliases = @{
    'syringe' = 'injector_1'
    'powder'  = 'gunpowder_1'
}
foreach ($aliasId in $aliases.Keys) {
    $targetId = $aliases[$aliasId]
    $target = $catalog | Where-Object { $_.Id -eq $targetId } | Select-Object -First 1
    $source = $catalog | Where-Object { $_.Id -eq $aliasId } | Select-Object -First 1
    if (-not $target -or -not $source) { continue }
    $source.Name = $target.Name
    $source.Sprite = $target.Sprite
    $source.Rarity = $target.Rarity
    $source.Cat = $target.Cat
}

# --- placement prefixes ----------------------------------------------------
# A whole family of placement ids can share one item: every "original_picture_N"
# in the world is the same Painting item. Mapping the prefix avoids listing each.
$prefixAliases = @{
    'original_picture' = 'picture_tube_1'
}
foreach ($row in $catalog) {
    foreach ($prefix in $prefixAliases.Keys) {
        if (-not $row.Id.StartsWith($prefix)) { continue }
        $target = $catalog | Where-Object { $_.Id -eq $prefixAliases[$prefix] } | Select-Object -First 1
        if (-not $target) { continue }
        $row.Name = $target.Name
        $row.Sprite = $target.Sprite
        $row.Rarity = $target.Rarity
        $row.Cat = $target.Cat
    }
}

# --- price and rarity resolution ------------------------------------------
$rarityOrder = @{ 'Legendary' = 0; 'Epic' = 1; 'Rare' = 2; 'Uncommon' = 3; 'Common' = 4; 'Exotic' = 5 }
foreach ($row in $catalog) {
    $price = 0
    foreach ($key in @($row.Id.ToLowerInvariant(), $row.Match.ToLowerInvariant(), $row.Sprite.ToLowerInvariant())) {
        if ($key -and $prices.ContainsKey($key)) { $price = $prices[$key]; break }
    }
    if ($price -le 0) { $price = $row.DbPrice }
    $row | Add-Member -NotePropertyName Price -NotePropertyValue $price -Force

    $override = $null
    foreach ($key in @($row.Id.ToLowerInvariant(), $row.Match.ToLowerInvariant(), $row.Sprite.ToLowerInvariant())) {
        if ($key -and $rarityOverride.ContainsKey($key)) { $override = $rarityOverride[$key]; break }
    }
    if ($override) { $row.Rarity = $override }
}

# ---------------------------------------------------------------- emit
$catOrder = @{}
for ($i = 0; $i -lt $categoryLabels.Count; $i++) { $catOrder[$categoryLabels[$i]] = $i }

$hideCategories = @('Cargo', 'Containers', 'Valuables')

$sb = New-Object System.Text.StringBuilder
function W([string]$s) { [void]$sb.AppendLine($s) }

W '// Generated by tools/loot/gen_loot_catalog.ps1. Do not edit by hand.'
W '// Categories, names, rarity and prices come from the item database'
W '// (tools/loot/data/item-database.tsv); ids and entity kinds from the game registries.'
W '//'
W "// Rows: $($catalog.Count)   categories: $($categoryLabels.Count)"
W '#pragma once'
W '#include <array>'
W '#include <cstdint>'
W '#include <string_view>'
W '#include "../SDK/WorldEntities.h"'
W ''
W 'namespace LootFilter {'
W ''
W 'enum class Category : uint8_t {'
W (($categoryLabels | ForEach-Object { "    $($categoryId[$_])," }) -join "`n")
W '    COUNT'
W '};'
W ''
W 'inline constexpr const char* CategoryNames[] = {'
W (($categoryLabels | ForEach-Object { "    $(CStr $_)," }) -join "`n")
W '};'
W 'static_assert(std::size(CategoryNames) == static_cast<size_t>(Category::COUNT));'
W ''
W 'enum class Rarity : uint8_t {'
W '    Legendary = 0,'
W '    Epic,'
W '    Rare,'
W '    Uncommon,'
W '    Common,'
W '    Exotic,'
W '    COUNT'
W '};'
W ''
W 'inline constexpr const char* RarityNames[] = {'
W '    "Legendary", "Epic", "Rare", "Uncommon", "Common", "Exotic"'
W '};'
W 'static_assert(std::size(RarityNames) == static_cast<size_t>(Rarity::COUNT));'
W ''
W 'inline constexpr uint32_t RarityColors[] = {'
W '    0xFFFFD700,  // Legendary - gold'
W '    0xFFA335EE,  // Epic - purple'
W '    0xFF0070DD,  // Rare - blue'
W '    0xFF1EFF00,  // Uncommon - green'
W '    0xFFFFFFFF,  // Common - white'
W '    0xFFFF5A00,  // Exotic - orange'
W '};'
W 'static_assert(std::size(RarityColors) == static_cast<size_t>(Rarity::COUNT));'
W ''
W '// Per-item render flags. The menu shows these as a checkbox popup, so a row'
W '// can carry any combination instead of one fixed display style.'
W 'enum RenderFlags : uint8_t {'
W '    RenderIcon     = 1 << 0,'
W '    RenderName     = 1 << 1,'
W '    RenderDot      = 1 << 2,'
W '    RenderDistance = 1 << 3,'
W '    RenderPrice    = 1 << 4,'
W '    RenderInVehicle = 1 << 5,   // draw this item when it sits inside a vehicle'
W '    RenderInTable   = 1 << 6,   // draw this item when it sits in a container/table'
W ''
W '    RenderDefault  = RenderIcon | RenderName | RenderPrice,'
W '    RenderMAX      = 1 << 7'
W '};'
W ''
W 'inline constexpr const char* RenderFlagNames[] = {'
W '    "Icon", "Name", "Dot", "Distance", "Price", "In Vehicle", "In Table"'
W '};'
W 'inline constexpr uint8_t RenderFlagBits[] = {'
W '    RenderIcon, RenderName, RenderDot, RenderDistance, RenderPrice, RenderInVehicle, RenderInTable'
W '};'
W 'static_assert(std::size(RenderFlagNames) == 7);'
W ''
W 'struct ItemDef {'
W '    const char* id;'
W '    const char* displayName;'
W '    const char* iconSprite;'
W '    Category    category;'
W '    Rarity      rarity;'
W '    RobRunWorld::Kind kind;'
W '    const char* labelMatch;   // substring against entity.label ("" = any of kind)'
W ''
W '    uint8_t defaultR, defaultG, defaultB;'
W '    bool hideWhenEmpty;       // honour HideEmpty for this row (containers, cargo)'
W '    uint32_t price;           // market value in $, 0 when unknown'
W '};'
W ''
W '// Sorted by category, then rarity (legendary first) within each category.'
W 'inline constexpr std::array kItemCatalog = std::to_array<ItemDef>({'

$sorted = $catalog | Sort-Object @{ E = { $catOrder[$_.Cat] } }, @{ E = { $rarityOrder[$_.Rarity] } }, @{ E = { $_.Name.ToLowerInvariant() } }
$currentCat = $null
$iconCount = 0
foreach ($r in $sorted) {
    if ($r.Cat -ne $currentCat) {
        $currentCat = $r.Cat
        W ''
        W "// === $currentCat ==="
    }
    $rgb = if ($categoryColour.ContainsKey($r.Cat)) { $categoryColour[$r.Cat] } else { @(200,205,215) }
    $hide = if ($r.Cat -in $hideCategories) { 'true' } else { 'false' }
    if ($r.Sprite) { $iconCount++ }
    W ("    {{ {0}, {1}, {2}, Category::{3}, Rarity::{4}, RobRunWorld::Kind::{5}, {6}, {7}, {8}, {9}, {10}, {11} }}," -f `
        (CStr $r.Id), (CStr $r.Name), (CStr $r.Sprite), $categoryId[$r.Cat], $r.Rarity, $r.Kind, (CStr $r.Match), $rgb[0], $rgb[1], $rgb[2], $hide, $r.Price)
}
W '});'
W ''
W 'inline constexpr size_t kItemCount = kItemCatalog.size();'
W ''
W '} // namespace LootFilter'

[System.IO.File]::WriteAllText($OutFile, $sb.ToString(), (New-Object System.Text.UTF8Encoding($false)))
Write-Host ("registry  {0} rows from {1}" -f $registryRows, $Registry)
Write-Host ("catalog   {0} rows -> {1}" -f $sorted.Count, $OutFile)
Write-Host ("icons     {0} rows carry a sprite" -f $iconCount)
Write-Host ("priced    {0} rows carry a price" -f (@($sorted | Where-Object { $_.Price -gt 0 }).Count))
$sorted | Group-Object Cat | Sort-Object Name | ForEach-Object { Write-Host ("  {0,-24} {1}" -f $_.Name, $_.Count) }
