# build.ps1 - Build the miniSVM UEFI hypervisor with MSVC (VS2022) + NASM.
#
# Produces esp\EFI\BOOT\BOOTX64.EFI, the file UEFI firmware loads and runs.
# Usage:  powershell -ExecutionPolicy Bypass -File build.ps1
$ErrorActionPreference = 'Stop'
$root  = $PSScriptRoot
$src   = Join-Path $root 'src'
$build = Join-Path $root 'build'
$esp   = Join-Path $root 'esp\EFI\BOOT'
New-Item -ItemType Directory -Force -Path $build, $esp | Out-Null

# --- Locate and enter the Visual Studio 2022 x64 developer environment -------
# (PowerShell can't expand ${env:ProgramFiles(x86)} because of the parentheses,
#  so resolve the folder via .NET instead.)
$pf86    = [Environment]::GetFolderPath('ProgramFilesX86')
$vswhere = Join-Path $pf86 'Microsoft Visual Studio\Installer\vswhere.exe'
if (-not (Test-Path $vswhere)) { throw "vswhere.exe not found; is Visual Studio installed?" }
$vsPath = & $vswhere -latest -products * -property installationPath
if (-not $vsPath) { throw "No Visual Studio installation found." }
Import-Module (Join-Path $vsPath 'Common7\Tools\Microsoft.VisualStudio.DevShell.dll')
Enter-VsDevShell -VsInstallPath $vsPath -SkipAutomaticLocation `
    -DevCmdArguments '-arch=x64 -host_arch=x64' | Out-Null

# --- Source files ------------------------------------------------------------
# All .c files are compiled with cl.exe; all .asm files assembled with nasm
# (win64 COFF). Just drop a new source into src\ and it gets built.
#
# Skip Windows reserved device names (con, prn, aux, nul, com1-9, lpt1-9): a
# file whose base name is one of these can't be opened by normal Win32 APIs.
$reserved = 'con','prn','aux','nul','com1','com2','com3','com4','com5','com6',
            'com7','com8','com9','lpt1','lpt2','lpt3','lpt4','lpt5','lpt6',
            'lpt7','lpt8','lpt9'
$cFiles   = @(Get-ChildItem -Path $src -Filter *.c |
              Where-Object { $reserved -notcontains $_.BaseName.ToLower() } |
              ForEach-Object Name)
$asmFiles = @(Get-ChildItem -Path $src -Filter *.asm | ForEach-Object Name)

$objs = @()

# cl flags for a freestanding UEFI binary:
#   /GS-   no stack-security cookie (needs the CRT)
#   /Gs99999999  raise stack-probe threshold so we don't need __chkstk
#   /Zl    don't emit default-library directives
#   /O1    optimize for size; /Oi- keep intrinsics predictable
#   /std:c11  for static_assert
$clFlags = @('/nologo','/c','/W3','/GS-','/Gs99999999','/Zl','/O1','/Oi-',
             '/std:c11','/DMINISVM','/I', $src)

foreach ($c in $cFiles) {
    $obj = Join-Path $build ([IO.Path]::GetFileNameWithoutExtension($c) + '.obj')
    Write-Host "CC   $c"
    & cl.exe @clFlags "/Fo$obj" (Join-Path $src $c)
    if ($LASTEXITCODE -ne 0) { throw "cl.exe failed on $c" }
    $objs += $obj
}
foreach ($a in $asmFiles) {
    $obj = Join-Path $build ([IO.Path]::GetFileNameWithoutExtension($a) + '.obj')
    Write-Host "ASM  $a"
    & nasm.exe -f win64 (Join-Path $src $a) -o $obj
    if ($LASTEXITCODE -ne 0) { throw "nasm failed on $a" }
    $objs += $obj
}

# --- Link a UEFI application (PE32+, subsystem EFI_APPLICATION = 10) ----------
$efi = Join-Path $esp 'BOOTX64.EFI'
Write-Host "LINK BOOTX64.EFI"
& link.exe /nologo /OUT:$efi /SUBSYSTEM:EFI_APPLICATION /ENTRY:efi_main `
    /NODEFAULTLIB /MACHINE:X64 /DYNAMICBASE:NO @objs
if ($LASTEXITCODE -ne 0) { throw "link.exe failed" }

Write-Host "`nOK -> $efi"
