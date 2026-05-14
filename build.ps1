# build.ps1 — one-click build for win-ldac.
#
# Usage:
#   .\build.ps1                # Release build
#   .\build.ps1 -Config Debug  # Debug build
#   .\build.ps1 -Clean         # rm -rf build/ first
#
# Walks through:
#   1. Verify Visual Studio 2022 + CMake are reachable
#   2. Ensure git submodules (libldac, imgui, implot) are populated
#   3. Ensure vendor/btstack/ exists — clone if missing
#   4. Configure + build
#
# Doesn't do hardware setup (Zadig, Memory Integrity). See README.md.

[CmdletBinding()]
param(
    [ValidateSet('Release','Debug','RelWithDebInfo','MinSizeRel')]
    [string]$Config = 'Release',
    [switch]$Clean
)

$ErrorActionPreference = 'Stop'

function Section($msg) {
    Write-Host ""
    Write-Host "==> $msg" -ForegroundColor Cyan
}

function Fail($msg) {
    Write-Host "ERROR: $msg" -ForegroundColor Red
    exit 1
}

# ── Locate CMake ──────────────────────────────────────────────────
Section "Locating toolchain"

$cmake = (Get-Command cmake -ErrorAction SilentlyContinue).Source
if (-not $cmake) {
    # Fall back to the CMake bundled with VS 2022.
    $vsCmake = Get-ChildItem `
        "C:\Program Files\Microsoft Visual Studio\2022\*\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe" `
        -ErrorAction SilentlyContinue | Select-Object -First 1
    if ($vsCmake) {
        $cmake = $vsCmake.FullName
        Write-Host "Using CMake bundled with Visual Studio:`n  $cmake" -ForegroundColor DarkGray
    } else {
        Fail "CMake not found. Install Visual Studio 2022 with the C++ Desktop workload (which bundles CMake), or add a standalone CMake >= 3.16 to your PATH."
    }
} else {
    Write-Host "Using CMake on PATH: $cmake" -ForegroundColor DarkGray
}

# ── Submodules (libldac, imgui, implot) ──────────────────────────
Section "Initialising git submodules"

if (-not (Test-Path ".git")) {
    Fail "build.ps1 must be run from the repository root (no .git found)."
}

# vendor/ldacBT/libldac/inc/ldacBT.h is the canonical "submodule populated" check.
$needSubmodules = $false
if (-not (Test-Path "vendor/ldacBT/libldac/inc/ldacBT.h")) { $needSubmodules = $true }
if (-not (Test-Path "third_party/imgui/imgui.h"))           { $needSubmodules = $true }
if (-not (Test-Path "third_party/implot/implot.h"))         { $needSubmodules = $true }

if ($needSubmodules) {
    git submodule update --init --recursive
    if ($LASTEXITCODE -ne 0) {
        Fail "git submodule update failed."
    }
} else {
    Write-Host "All submodules already populated." -ForegroundColor DarkGray
}

# ── BTstack (user-fetched, NOT a submodule by design) ────────────
Section "Verifying BTstack"

if (-not (Test-Path "vendor/btstack/src/btstack.h")) {
    Write-Host "vendor/btstack/ is missing; cloning from upstream." -ForegroundColor Yellow
    Write-Host "Note: BTstack is free for personal use only. See LICENCE.md upstream." -ForegroundColor Yellow
    git clone https://github.com/bluekitchen/btstack.git vendor/btstack
    if ($LASTEXITCODE -ne 0) {
        Fail "Failed to clone BTstack."
    }
} else {
    Write-Host "BTstack present at vendor/btstack." -ForegroundColor DarkGray
}

# ── Build ────────────────────────────────────────────────────────
if ($Clean -and (Test-Path "build")) {
    Section "Cleaning build/"
    Remove-Item -Recurse -Force build
}

Section "Configuring CMake ($Config)"
& $cmake -S . -B build -G "Visual Studio 17 2022" -A x64
if ($LASTEXITCODE -ne 0) { Fail "CMake configure failed." }

Section "Building win-ldac ($Config)"
& $cmake --build build --config $Config --target win-ldac
if ($LASTEXITCODE -ne 0) { Fail "CMake build failed." }

# ── Summary ──────────────────────────────────────────────────────
$exe = "build\gui\$Config\win-ldac.exe"
if (Test-Path $exe) {
    $sz = [math]::Round((Get-Item $exe).Length / 1KB, 1)
    Write-Host ""
    Write-Host "Built: $exe ($sz KB)" -ForegroundColor Green
    Write-Host ""
    Write-Host "Next steps:" -ForegroundColor Cyan
    Write-Host "  1. Run Zadig and swap the BT dongle driver to WinUSB."
    Write-Host "  2. Put your headphones into pairing mode."
    Write-Host "  3. Launch: $exe"
} else {
    Fail "Expected output $exe not found after build."
}
