<#
.SYNOPSIS
    Wire the TempleOS-HL1 mod sources into a Half-Life SDK client build.

.DESCRIPTION
    The mod compiles into the Half-Life SDK's client library. This script
    applies integration/halflife-updated.patch to a checkout of
    SamVanheer/halflife-updated (steam_legacy branch), which:

      - adds three #include "toshl_hooks.h" splices and the mod entry-point
        calls to cdll_int.cpp, input.cpp and tri.cpp,
      - adds the mod sources (this repo's src/ and third_party/minhook) plus
        the SDK glue in src/sdk_glue to hl_cdll.vcxproj,
      - points filecopy.bat at the half-life-templeos mod folder.

    The project references the mod by the relative path
    ..\..\..\half-life-templeos, so this repo must be checked out next to the
    SDK tree (same parent folder) and keep the folder name half-life-templeos.

.PARAMETER SdkPath
    Path to your halflife-updated checkout (steam_legacy branch, at or after
    commit edbae22).

.PARAMETER Revert
    Undo a previously applied integration instead of applying it.

.EXAMPLE
    .\integration\apply-integration.ps1 -SdkPath ..\halflife-updated
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string]$SdkPath,
    [switch]$Revert
)

$ErrorActionPreference = 'Stop'

$repoRoot = Split-Path $PSScriptRoot -Parent
$patch = Join-Path $PSScriptRoot 'halflife-updated.patch'

if (-not (Test-Path $patch)) {
    throw "Patch not found at $patch"
}

$sdk = (Resolve-Path $SdkPath).Path
if (-not (Test-Path (Join-Path $sdk 'cl_dll\cdll_int.cpp'))) {
    throw "$sdk does not look like a Half-Life SDK checkout (no cl_dll\cdll_int.cpp)."
}

# The vcxproj references ..\..\..\half-life-templeos, so the repo must sit next
# to the SDK tree and keep its folder name. Warn (do not fail) if it does not.
if ((Split-Path $repoRoot -Leaf) -ne 'half-life-templeos') {
    Write-Warning "This repo folder is '$(Split-Path $repoRoot -Leaf)', not 'half-life-templeos'. The project's relative paths will not resolve until it is renamed."
}
if ((Split-Path $repoRoot -Parent) -ne (Split-Path $sdk -Parent)) {
    Write-Warning "This repo and the SDK checkout are not in the same parent folder. Put them side by side or the project's relative paths will not resolve."
}

Push-Location $sdk
try {
    if ($Revert) {
        git apply --reverse --check -- "$patch"
        git apply --reverse -- "$patch"
        Write-Host "Reverted the TempleOS-HL1 integration from $sdk"
        return
    }

    # Fail early and clearly if it will not apply cleanly (already applied,
    # wrong base commit, or a modified SDK).
    & git apply --check -- "$patch" 2>$null
    if ($LASTEXITCODE -ne 0) {
        & git apply --reverse --check -- "$patch" 2>$null
        if ($LASTEXITCODE -eq 0) {
            Write-Host "Integration is already applied to $sdk. Nothing to do."
            return
        }
        throw "Patch does not apply cleanly. Check that $sdk is on the steam_legacy branch at (or based on) commit edbae22 with no conflicting local edits."
    }

    git apply -- "$patch"
    Write-Host ""
    Write-Host "Applied the TempleOS-HL1 integration to $sdk"
    Write-Host ""
    Write-Host "Next:"
    Write-Host "  1. Open projects\vs2019\projects.sln in Visual Studio 2019."
    Write-Host "  2. Build hl_cdll in Release / Win32 (v142 toolset)."
    Write-Host "  3. The post-build step copies client.dll into the mod folder set in filecopy.bat."
}
finally {
    Pop-Location
}
