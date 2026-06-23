# Apotheosis macro layer is built against Lua 5.1.5 (the same major.minor
# series Logitech G HUB / LGS uses, so user scripts copy-pasted from there
# will run unmodified). This script downloads the official tarball from
# www.lua.org and extracts ONLY the C sources + headers we need into
# .\src\, leaving the existing CMake glob in CMakeLists.txt happy.
#
# Run this once after pulling the repo:
#   powershell -NoProfile -ExecutionPolicy Bypass -File .\fetch_lua.ps1
#
# Re-runs are idempotent (skips download if .\lua-5.1.5.tar.gz already
# exists; skips extraction if .\src\lua.h already exists).

$ErrorActionPreference = 'Stop'
Set-Location -Path $PSScriptRoot

$archive  = 'lua-5.1.5.tar.gz'
$url      = 'https://www.lua.org/ftp/lua-5.1.5.tar.gz'
$srcDir   = Join-Path $PSScriptRoot 'src'
$probe    = Join-Path $srcDir       'lua.h'

if (Test-Path $probe) {
    Write-Host "[fetch_lua] $probe already exists. Nothing to do."
    exit 0
}

if (-not (Test-Path $archive)) {
    Write-Host "[fetch_lua] Downloading $url ..."
    try {
        Invoke-WebRequest -Uri $url -OutFile $archive -UseBasicParsing
    } catch {
        Write-Error @"
[fetch_lua] Download failed: $($_.Exception.Message)

Manual fallback:
  1. Get $archive from $url
  2. Place it next to this script ($PSScriptRoot)
  3. Re-run this script.
"@
        exit 1
    }
}

# Use built-in tar (Windows 10/11 ships bsdtar as tar.exe) to extract.
$tar = Get-Command tar.exe -ErrorAction SilentlyContinue
if (-not $tar) {
    Write-Error "[fetch_lua] tar.exe not found. Windows 10 1803+ ships it. Install or extract $archive manually so that .\src\lua.h exists."
    exit 1
}

Write-Host "[fetch_lua] Extracting $archive ..."
& $tar.Source -xzf $archive
if ($LASTEXITCODE -ne 0) {
    Write-Error "[fetch_lua] tar -xzf failed."
    exit 1
}

$extracted = Join-Path $PSScriptRoot 'lua-5.1.5'
if (-not (Test-Path $extracted)) {
    Write-Error "[fetch_lua] Expected $extracted after extraction; not found."
    exit 1
}

if (-not (Test-Path $srcDir)) {
    New-Item -ItemType Directory -Path $srcDir | Out-Null
}

Write-Host "[fetch_lua] Copying sources to .\src ..."
Copy-Item -Path (Join-Path $extracted 'src\*.c') -Destination $srcDir -Force
Copy-Item -Path (Join-Path $extracted 'src\*.h') -Destination $srcDir -Force

# We don't ship the lua / luac front-ends. Both pull stuff we don't need
# and bring conflicting main()s.
Remove-Item -Path (Join-Path $srcDir 'lua.c')  -Force -ErrorAction SilentlyContinue
Remove-Item -Path (Join-Path $srcDir 'luac.c') -Force -ErrorAction SilentlyContinue
Remove-Item -Path (Join-Path $srcDir 'print.c') -Force -ErrorAction SilentlyContinue

# Copy the license alongside the sources so the vendor tree carries it.
Copy-Item -Path (Join-Path $extracted 'COPYRIGHT') -Destination (Join-Path $PSScriptRoot 'COPYRIGHT') -Force -ErrorAction SilentlyContinue

# Cleanup the extracted tree; keep the tarball so re-runs without network
# still work.
Remove-Item -Path $extracted -Recurse -Force

Write-Host "[fetch_lua] Done. Lua 5.1.5 sources are in $srcDir."
