<#
.SYNOPSIS
    Builds, stages runtime assets, and packages VeoxDMA for release.

.DESCRIPTION
    1. MSBuild VeoxDMA.sln (Release|x64).
    2. Copies runtime assets next to VeoxDMA.exe:
         - city_colliders.bvh (from tools\)
         - maps\*.png          (from maps\)
       vmm.dll / leechcore.dll / FTD3XX.dll are emitted by the build itself.
    3. Zips the publish folder into _upload\VeoxDMA.zip.

.PARAMETER MSBuild
    Full path to MSBuild.exe. Resolved via vswhere if not supplied.

.PARAMETER NoZip
    Skip the zip step (fast dev loop: just build + stage).

.PARAMETER NoOpen
    Do not open the output folder when done.
#>
[CmdletBinding()]
param(
    [string]$MSBuild,
    [switch]$NoZip,
    [switch]$NoOpen
)

$ErrorActionPreference = "Stop"

$RepoRoot   = $PSScriptRoot
Set-Location $RepoRoot

$Sln        = Join-Path $RepoRoot "VeoxDMA.sln"
$PublishDir = Join-Path $RepoRoot "Build\Release\VeoxDMA"
$ExeName    = "VeoxDMA.exe"
$PublishExe = Join-Path $PublishDir $ExeName
$LibDir     = Join-Path $RepoRoot "Lib"
$RuntimeDlls = @("vmm.dll", "leechcore.dll", "FTD3XX.dll")
$BvhSrc     = Join-Path $RepoRoot "tools\city_colliders.bvh"
$MapsSrc    = Join-Path $RepoRoot "maps"
$MapsDst    = Join-Path $PublishDir "maps"
$UploadDir  = Join-Path $RepoRoot "_upload"
$ZipPath    = Join-Path $UploadDir "VeoxDMA.zip"

function Write-Step($msg) {
    Write-Host "`n==> $msg" -ForegroundColor Cyan
}

# ---- 0. resolve MSBuild -----------------------------------------------------
# Try (in order): -MSBuild override, vswhere (incl. prerelease/Insiders),
# glob of the usual VS install roots. The Microsoft.Component.MSBuild
# requirement is deliberately NOT passed to vswhere: Insiders doesn't
# advertise it the same way and gets filtered out.
if (-not $MSBuild) {
    $vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
    if (Test-Path $vswhere) {
        $MSBuild = & $vswhere -latest -prerelease -products '*' `
            -find "MSBuild\**\Bin\MSBuild.exe" | Select-Object -First 1
    }
}
if (-not $MSBuild -or -not (Test-Path $MSBuild)) {
    $candidates = @(
        "${env:ProgramFiles}\Microsoft Visual Studio",
        "${env:ProgramFiles(x86)}\Microsoft Visual Studio"
    ) | Where-Object { Test-Path $_ } | ForEach-Object {
        Get-ChildItem $_ -Recurse -Filter MSBuild.exe -ErrorAction SilentlyContinue |
            Where-Object { $_.FullName -match '\\MSBuild\\.+\\Bin\\MSBuild\.exe$' -and
                           $_.FullName -notmatch '\\amd64\\' }
    }
    $MSBuild = $candidates | Select-Object -ExpandProperty FullName -First 1
}
if (-not $MSBuild -or -not (Test-Path $MSBuild)) {
    throw "MSBuild.exe not found. Install VS 2022/2026 (or Build Tools) or pass -MSBuild <path>."
}
Write-Host "MSBuild: $MSBuild" -ForegroundColor DarkGray

# ---- 1. build ---------------------------------------------------------------
# Wipe the publish folder so stale files (an old .bvh, removed maps, a renamed
# dll) can never ride along into the zip.
if (Test-Path $PublishDir) { Remove-Item $PublishDir -Recurse -Force }

Write-Step "Building Release|x64 ..."
# PlatformToolset is NOT overridden: the vcxproj pins v143, and forcing a
# value here would fail on machines whose VS install lacks that toolset.
& $MSBuild $Sln /p:Configuration=Release /p:Platform=x64 /m /nologo /v:m
if ($LASTEXITCODE -ne 0) { throw "MSBuild failed with exit code $LASTEXITCODE" }
if (-not (Test-Path $PublishExe)) { throw "Build did not produce '$PublishExe'" }

# ---- 2. stage runtime assets ------------------------------------------------
# MSBuild only emits VeoxDMA.exe; the project links against vmm.lib /
# leechcore.lib / FTD3XX.lib but does NOT copy their runtime DLLs. Those
# three DLLs, the BVH, and the radar PNGs are all repo assets we stage
# here, so the zip is a drop-in run folder.
Write-Step "Staging runtime assets ..."

foreach ($dll in $RuntimeDlls) {
    $src = Join-Path $LibDir $dll
    if (-not (Test-Path $src)) {
        throw "Runtime DLL '$dll' not found at '$src'. Copy a matching MemProcFS build into Lib\."
    }
    Copy-Item $src (Join-Path $PublishDir $dll) -Force
}

if (-not (Test-Path $BvhSrc)) {
    throw "city_colliders.bvh not found at '$BvhSrc' - check the tools\ folder."
}
Copy-Item $BvhSrc (Join-Path $PublishDir "city_colliders.bvh") -Force

if (-not (Test-Path $MapsSrc)) {
    throw "maps\ source folder not found at '$MapsSrc'."
}
New-Item -ItemType Directory -Force -Path $MapsDst | Out-Null
Copy-Item (Join-Path $MapsSrc "*.png") $MapsDst -Force
$mapCount = (Get-ChildItem $MapsDst -Filter *.png).Count
if ($mapCount -eq 0) { throw "No radar PNGs were copied from '$MapsSrc'." }

# info.db is a MemProcFS symbol cache written at runtime; it does not belong
# in a release zip. Remove it if a previous unclean build left one behind.
$StaleInfoDb = Join-Path $PublishDir "info.db"
if (Test-Path $StaleInfoDb) { Remove-Item $StaleInfoDb -Force }

# Strip debug symbols: MSVC Release still emits VeoxDMA.pdb, and the DMA
# DLLs can arrive with their own .pdb siblings. They are not needed at
# runtime and leak symbol names / build paths through the zip.
$pdbs = Get-ChildItem $PublishDir -Recurse -Filter *.pdb -File
if ($pdbs) {
    Write-Host ("  stripping {0} .pdb file(s)" -f $pdbs.Count)
    $pdbs | Remove-Item -Force
}

Write-Host "  exe   : $ExeName"
Write-Host "  dlls  : $($RuntimeDlls -join ', ')"
Write-Host "  bvh   : city_colliders.bvh"
Write-Host "  maps  : $mapCount file(s)"

# ---- 3. zip -----------------------------------------------------------------
if ($NoZip) {
    Write-Step "Done (publish only): $PublishDir"
    if (-not $NoOpen) { Start-Process explorer.exe $PublishDir }
    return
}

New-Item -ItemType Directory -Force -Path $UploadDir | Out-Null
if (Test-Path $ZipPath) { Remove-Item $ZipPath -Force }

Write-Step "Zipping publish folder ..."
Compress-Archive -Path (Join-Path $PublishDir "*") -DestinationPath $ZipPath -CompressionLevel Optimal

Write-Step "Done: $ZipPath"
Get-Item $ZipPath | Select-Object FullName, @{N = "SizeMB"; E = { [math]::Round($_.Length / 1MB, 1) } }

if (-not $NoOpen) { Start-Process explorer.exe $UploadDir }
