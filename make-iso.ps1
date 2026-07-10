# make-iso.ps1 - Build a UEFI-bootable ISO and validate it in QEMU.
#
# Pipeline: build BOOTX64.EFI -> generate a FAT16 ESP image (make_esp.py) ->
# wrap it into an El Torito UEFI ISO (xorriso) -> boot the ISO in QEMU.
# The resulting miniSVM.iso can be attached to a VMware VM's CD/DVD drive.
#
# Usage:  powershell -ExecutionPolicy Bypass -File make-iso.ps1
$ErrorActionPreference = 'Stop'
$root = $PSScriptRoot

# 1) Compile
& (Join-Path $root 'build.ps1')
$efi = Join-Path $root 'esp\EFI\BOOT\BOOTX64.EFI'
if (-not (Test-Path $efi)) { throw "BOOTX64.EFI not built" }

# 2) Lay out the ISO tree: the FAT ESP image (El Torito boot image) plus a copy
#    of the EFI file on the ISO9660 filesystem itself (some firmwares look there).
$isoRoot = Join-Path $root 'build\isoroot'
$bootDir = Join-Path $isoRoot 'EFI\BOOT'
New-Item -ItemType Directory -Force -Path $bootDir | Out-Null
Copy-Item $efi (Join-Path $bootDir 'BOOTX64.EFI') -Force

$espImg = Join-Path $isoRoot 'esp.img'
Write-Host "ESP  make_esp.py"
& python (Join-Path $root 'tools\make_esp.py') $efi $espImg
if ($LASTEXITCODE -ne 0) { throw "make_esp.py failed" }

# 3) Build the ISO. -eltorito-platform efi marks the boot image as UEFI (0xEF).
#    xorriso here is a Cygwin build, so it needs POSIX paths (/cygdrive/c/...)
#    rather than Windows paths (C:\...).
function To-Cyg([string]$p) {
    $full = [IO.Path]::GetFullPath($p)
    $drive = $full.Substring(0, 1).ToLower()
    $rest = ($full.Substring(2) -replace '\\', '/')
    return "/cygdrive/$drive$rest"
}
$iso = Join-Path $root 'miniSVM.iso'
Write-Host "ISO  xorriso"
& xorriso -as mkisofs -V MINISVM -J -r `
    -eltorito-platform efi -e esp.img -no-emul-boot `
    -o (To-Cyg $iso) (To-Cyg $isoRoot)
if ($LASTEXITCODE -ne 0) { throw "xorriso failed" }
Write-Host "`nOK -> $iso`n"

# 4) Validate: boot the ISO in QEMU (UEFI) and print the serial console.
$qemu = (Get-Command qemu-system-x86_64 -ErrorAction SilentlyContinue).Source
if (-not $qemu) { $qemu = 'C:\Program Files\qemu\qemu-system-x86_64.exe' }
$code = Join-Path (Split-Path $qemu) 'share\edk2-x86_64-code.fd'
$log  = Join-Path $root 'build\serial-iso.log'
$a = "-machine q35 -m 256 -cpu qemu64,+svm " +
     "-drive `"if=pflash,format=raw,readonly=on,file=$code`" " +
     "-cdrom `"$iso`" " +
     "-serial `"file:$log`" -display none -no-reboot"

Write-Host "Booting the ISO in QEMU (15s time-box)...`n"
$p = Start-Process $qemu -ArgumentList $a -PassThru -WindowStyle Hidden
Start-Sleep -Seconds 15
if (-not $p.HasExited) { Stop-Process -Id $p.Id -Force }

Write-Host "==================== ISO serial output ===================="
if (Test-Path $log) { Get-Content $log } else { Write-Host "(no serial output - ISO may not have booted)" }
Write-Host "==========================================================="
