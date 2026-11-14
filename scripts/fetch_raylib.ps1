# Downloads the prebuilt raylib release for MSVC x64 into third_party/raylib.
# Usage: powershell -ExecutionPolicy Bypass -File scripts/fetch_raylib.ps1

$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'

$version = '6.2'
$name    = "raylib-${version}_win64_msvc16"
$sha256  = '8467b5dd6db4d7ab942f3148a3d30d93be6ed9b2fab88069d177e59604268ebd'

$root   = Split-Path -Parent $PSScriptRoot
$dest   = Join-Path $root 'third_party'
$target = Join-Path $dest 'raylib'

if (Test-Path (Join-Path $target 'include\raylib.h')) {
    Write-Host "raylib $version already present in third_party/raylib"
    exit 0
}

New-Item -ItemType Directory -Force $dest | Out-Null
$zip = Join-Path $dest "$name.zip"
$url = "https://github.com/raysan5/raylib/releases/download/$version/$name.zip"

Write-Host "Downloading $url"
Invoke-WebRequest -Uri $url -OutFile $zip -UseBasicParsing

$actual = (Get-FileHash -Algorithm SHA256 $zip).Hash.ToLower()
if ($sha256 -and ($actual -ne $sha256)) {
    Remove-Item $zip
    throw "SHA-256 mismatch for $name.zip: expected $sha256, got $actual"
}
Write-Host "SHA-256: $actual"

Expand-Archive -Path $zip -DestinationPath $dest -Force
if (Test-Path $target) { Remove-Item -Recurse -Force $target }
Rename-Item (Join-Path $dest $name) 'raylib'
Remove-Item $zip

Write-Host "raylib $version ready in third_party/raylib"
