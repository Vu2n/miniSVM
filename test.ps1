# test.ps1 - Build, then boot in QEMU and print the serial console.
#
# One-shot: compiles BOOTX64.EFI, runs it under OVMF in QEMU for a few seconds,
# then dumps whatever the hypervisor printed. Handy for the M1-M3 milestones,
# which end by halting (so we time-box QEMU instead of waiting for it to exit).
#
# Usage:  powershell -ExecutionPolicy Bypass -File test.ps1
$ErrorActionPreference = 'Stop'
$root = $PSScriptRoot

# 1) Build
& (Join-Path $root 'build.ps1')

# 2) Boot in QEMU with the serial console redirected to a log file
$qemu = (Get-Command qemu-system-x86_64 -ErrorAction SilentlyContinue).Source
if (-not $qemu) { $qemu = 'C:\Program Files\qemu\qemu-system-x86_64.exe' }
$code = Join-Path (Split-Path $qemu) 'share\edk2-x86_64-code.fd'
$esp  = Join-Path $root 'esp'
$log  = Join-Path $root 'build\serial.log'
if (Test-Path $log) { Clear-Content $log }

$a = "-machine q35 -m 256 -cpu qemu64,+svm " +
     "-drive `"if=pflash,format=raw,readonly=on,file=$code`" " +
     "-drive `"format=raw,file=fat:rw:$esp`" " +
     "-serial `"file:$log`" -display none -no-reboot"

Write-Host "`nBooting in QEMU (15s time-box)...`n"
$p = Start-Process $qemu -ArgumentList $a -PassThru -WindowStyle Hidden
Start-Sleep -Seconds 15
if (-not $p.HasExited) { Stop-Process -Id $p.Id -Force }

Write-Host "==================== serial output ===================="
if (Test-Path $log) { Get-Content $log } else { Write-Host "(no serial output produced)" }
Write-Host "======================================================="
