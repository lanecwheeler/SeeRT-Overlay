# Packages a built SeeRT Overlay into the release zip - the app and the files
# it loads from its own folder, plus licences - with its SHA-256 and release
# notes.  The release workflow (.github/workflows/release.yml) runs it after
# building; it runs locally too:
#
#   powershell -ExecutionPolicy Bypass -File tools\package-release.ps1
#
# Output in dist\:  SeeRT-Overlay-vX.Y.Z-win64.zip, its .sha256, release-notes.md
# The version comes from project(... VERSION) in CMakeLists.txt.

param(
    [string]$BinDir = "build\bin\Release",   # relative to the repo root
    [string]$OutDir = "dist"
)

$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent

$cmake = Get-Content (Join-Path $root 'CMakeLists.txt') -Raw
if ($cmake -notmatch 'project\(SeeRTOverlay VERSION (\d+\.\d+\.\d+)') {
    throw 'No project(SeeRTOverlay VERSION x.y.z) in CMakeLists.txt'
}
$version = $Matches[1]
$name    = "SeeRT-Overlay-v$version-win64"
$bin     = Join-Path $root $BinDir
$out     = Join-Path $root $OutDir
$stage   = Join-Path $out $name

if (Test-Path $stage) { Remove-Item -Recurse -Force $stage }
New-Item -ItemType Directory -Force $stage | Out-Null
New-Item -ItemType Directory -Force (Join-Path $stage 'licenses') | Out-Null

# The app and what it loads from its own folder: compiled shaders, fonts and
# the cursor image.  (Not imgui.ini or anything else a run leaves behind.)
foreach ($required in 'SeeRTOverlay.exe', 'cursor.png') {
    if (-not (Test-Path (Join-Path $bin $required))) { throw "Missing $required in $bin - build first" }
}
$appFiles = @(Get-Item (Join-Path $bin 'SeeRTOverlay.exe'), (Join-Path $bin 'cursor.png')) +
            @(Get-ChildItem (Join-Path $bin '*') -Include '*.cso', '*.ttf', '*.otf' -File)
if (-not ($appFiles | Where-Object Extension -eq '.cso')) { throw "No compiled shaders (.cso) in $bin" }
$appFiles | Copy-Item -Destination $stage

# Licences: the app's, and each bundled font's and icon's.
Copy-Item (Join-Path $root 'LICENSE.txt') $stage
Get-ChildItem (Join-Path $root 'src\render\assets') -Include '*-OFL.txt', '*-LICENSE.txt' -File -Recurse |
    Copy-Item -Destination (Join-Path $stage 'licenses')

# Zip (the files at the top level, like earlier releases) and checksum.
$zip = Join-Path $out "$name.zip"
if (Test-Path $zip) { Remove-Item -Force $zip }
Compress-Archive -Path (Join-Path $stage '*') -DestinationPath $zip
$hash = (Get-FileHash $zip -Algorithm SHA256).Hash.ToLower()
Set-Content -Path "$zip.sha256" -Value "$hash  $name.zip" -Encoding ascii

$notes = @"
SeeRT Overlay v$version for Windows 10 and 11 (64-bit).

Download **$name.zip**, unzip it anywhere and run ``SeeRTOverlay.exe``. Settings and presets live in ``%APPDATA%\SeeRT Overlay``, so they carry over from earlier versions.

- Needs the [Microsoft Visual C++ Redistributable (x64)](https://aka.ms/vs/17/release/vc_redist.x64.exe) - most PCs already have it.
- The app isn't code-signed, so Windows SmartScreen may warn the first time you run it.

SHA-256 of ``$name.zip``: ``$hash``
"@
Set-Content -Path (Join-Path $out 'release-notes.md') -Value $notes -Encoding utf8

Write-Host "Packaged $zip"
Write-Host "SHA-256  $hash"

# Hand the results to the workflow's later steps.
if ($env:GITHUB_OUTPUT) {
    Add-Content -Path $env:GITHUB_OUTPUT -Value "version=$version" -Encoding utf8
    Add-Content -Path $env:GITHUB_OUTPUT -Value "zip=$zip" -Encoding utf8
}
