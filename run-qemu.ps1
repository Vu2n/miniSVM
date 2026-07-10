# run-qemu.ps1 - Boot the built hypervisor in QEMU (OVMF UEFI firmware).
#
# QEMU emulates AMD-V/SVM in software (TCG), so this works on any host CPU and
# lets you test the full hypervisor before touching VMware. The firmware mirrors
# the UEFI console to the serial port, which we send to this terminal.
#
# Usage:  powershell -ExecutionPolicy Bypass -File run-qemu.ps1
$ErrorActionPreference = 'Stop'
$root = $PSScriptRoot
$esp  = Join-Path $root 'esp'
if (-not (Test-Path (Join-Path $esp 'EFI\BOOT\BOOTX64.EFI'))) {
    throw "BOOTX64.EFI not found - run build.ps1 first."
}

$qemu = (Get-Command qemu-system-x86_64 -ErrorAction SilentlyContinue).Source
if (-not $qemu) { $qemu = 'C:\Program Files\qemu\qemu-system-x86_64.exe' }
$code = Join-Path (Split-Path $qemu) 'share\edk2-x86_64-code.fd'
if (-not (Test-Path $code)) { throw "OVMF firmware not found at $code" }

# We boot with just the read-only OVMF code image (no separate NVRAM varstore).
# The firmware prints a harmless warning about not being able to save variables,
# then boots our BOOTX64.EFI off the FAT disk (the esp\ directory via vvfat).
#
# Paths may contain spaces, so pass a single quoted command-line string; QEMU
# splits -drive options on commas, not spaces.
$a = "-machine q35 -m 256 -cpu qemu64,+svm " +
     "-drive `"if=pflash,format=raw,readonly=on,file=$code`" " +
     "-drive `"format=raw,file=fat:rw:$esp`" " +
     "-serial stdio -display none -no-reboot"

Write-Host "Launching QEMU (Ctrl+C to quit)...`n"
Start-Process $qemu -ArgumentList $a -NoNewWindow -Wait
