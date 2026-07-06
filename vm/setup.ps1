# setup.ps1 - fetch TempleOS ISO + point the mod at your QEMU install.
# Run from the vm/ directory:  powershell -ExecutionPolicy Bypass -File setup.ps1

$ErrorActionPreference = "Stop"

# --- TempleOS ISO (public domain) ---------------------------------------
# Terry A. Davis released TempleOS into the public domain. The canonical
# distribution is the ISO from templeos.org (mirrored widely). Update the
# URL if the mirror rots; any TempleOS 5.03 ISO works.
$isoUrl  = "https://templeos.org/Downloads/TempleOS.ISO"
$isoPath = Join-Path $PSScriptRoot "TempleOS.iso"

if (-not (Test-Path $isoPath)) {
    Write-Host "Downloading TempleOS ISO..."
    try {
        Invoke-WebRequest -Uri $isoUrl -OutFile $isoPath
        Write-Host "Saved $isoPath"
    } catch {
        Write-Warning "Auto-download failed. Manually place TempleOS.iso in this folder."
        Write-Warning "Mirror list: https://templeos.org  /  archive.org 'TempleOS'"
    }
} else {
    Write-Host "TempleOS.iso already present."
}

# --- QEMU location -------------------------------------------------------
# We don't bundle QEMU (GPL). Point the mod at your install so the launcher
# can find it without relying on PATH.
$qemuGuess = @(
    "C:\Program Files\qemu\qemu-system-x86_64.exe",
    "C:\qemu\qemu-system-x86_64.exe"
) | Where-Object { Test-Path $_ } | Select-Object -First 1

$qemuPathFile = Join-Path $PSScriptRoot "qemu_path.txt"
if ($qemuGuess) {
    Set-Content -Path $qemuPathFile -Value $qemuGuess -NoNewline
    Write-Host "Wrote qemu_path.txt -> $qemuGuess"
} else {
    Write-Warning "QEMU not found. Install it (https://www.qemu.org/download/#windows)"
    Write-Warning "then put the full path to qemu-system-x86_64.exe in vm/qemu_path.txt"
}

Write-Host ""
Write-Host "Test the VM standalone before touching the game:"
Write-Host "  `"$qemuGuess`" -m 512 -drive file=TempleOS.iso,media=cdrom -boot d -snapshot -vnc 127.0.0.1:0"
Write-Host "then run tools\rfb_probe.exe 127.0.0.1 5900 frame.ppm"
