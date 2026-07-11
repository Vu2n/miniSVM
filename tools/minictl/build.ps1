# build.ps1 - build minictl.exe (run it INSIDE the Windows guest).
$ErrorActionPreference = 'Stop'
$root = $PSScriptRoot

$pf86    = [Environment]::GetFolderPath('ProgramFilesX86')
$vswhere = Join-Path $pf86 'Microsoft Visual Studio\Installer\vswhere.exe'
$vsPath  = & $vswhere -latest -products * -property installationPath
Import-Module (Join-Path $vsPath 'Common7\Tools\Microsoft.VisualStudio.DevShell.dll')
Enter-VsDevShell -VsInstallPath $vsPath -SkipAutomaticLocation `
    -DevCmdArguments '-arch=x64 -host_arch=x64' | Out-Null

# Run from the tool directory so filenames have no spaces (avoids ml64/cl path
# parsing issues with "3D Objects").
Push-Location $root
try {
    & ml64.exe /nologo /c /Fohvcall.obj hvcall.asm
    if ($LASTEXITCODE -ne 0) { throw "ml64 failed" }
    & cl.exe /nologo minictl.c hvcall.obj /Feminictl.exe /Fominictl.obj
    if ($LASTEXITCODE -ne 0) { throw "cl failed" }
} finally {
    Pop-Location
}

Write-Host "`nOK -> $root\minictl.exe"
Write-Host "Copy minictl.exe into the Windows guest and run it there."
