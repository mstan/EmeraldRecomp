<#
make_release.ps1 - build every EmeraldRecomp release artifact.

  release-stage\EmeraldRecomp-windows-x64-v<Version>.zip
  release-stage\EmeraldRecomp-linux-x86_64-v<Version>.AppImage
  release-stage\EmeraldRecomp-android-arm64-v<Version>.apk
  release-stage\SHA256SUMS.txt

Every artifact is bring-your-own-ROM: the ROM and the GBA BIOS are never
bundled. Each is scanned for private assets (by name, size and the GBA
cartridge header) before it is accepted, and the desktop builds are smoke-run
headless against your local ROM + BIOS (must report FULLY_STATIC).

Steps:
  1. Pins: the engine (-EngineRoot) and recomp-ui (-RecompUiRoot) checkouts
     must be at exactly the commits this repo pins, with no uncommitted build
     inputs; this repo must have no uncommitted tracked changes.
  2. Build the engine's gba_recompile and regenerate variants/emerald/generated
     from your ROM (generated C is never committed; it is derived per build).
  3. Windows (MinGW Release) zip.
  4. Linux AppImage via WSL + Docker (tools/linux/make_appimage.sh; Ubuntu 22.04
     builder, engine-pinned SDL bundled).
  5. Android release APK (arm64). Signed with a release key when
     GBARECOMP_KEYSTORE / GBARECOMP_KEYSTORE_PASSWORD / GBARECOMP_KEY_ALIAS
     (and optionally GBARECOMP_KEY_PASSWORD) are set; otherwise the local
     debug key, with a loud warning.

Publish via gh AFTER the user signs off:

  gh release create v<Version> release-stage\EmeraldRecomp-*-v<Version>.* release-stage\SHA256SUMS.txt

Usage:
  powershell -File tools\make_release.ps1 -Version 0.0.7 -RecompUiRoot <recomp-ui checkout>
  powershell -File tools\make_release.ps1 -Version 0.0.7 -Platforms windows,android
#>
param(
  [Parameter(Mandatory = $true)][string]$Version,
  [ValidateSet('windows', 'linux', 'android')][string[]]$Platforms = @('windows', 'linux', 'android'),
  [string]$EngineRoot,
  [string]$RecompUiRoot,
  [string]$Rom,
  [string]$Bios,
  [int]$Jobs = 8,
  [switch]$AllowDirty
)
$ErrorActionPreference = 'Stop'
if ($Version -notmatch '^\d+\.\d+\.\d+$') { throw "Version must be X.Y.Z (got '$Version')" }

$MingwBin = 'C:\msys64\mingw64\bin'
$env:PATH = "$MingwBin;$env:PATH"
$root = Split-Path -Parent $PSScriptRoot
if (-not $EngineRoot) { $EngineRoot = Join-Path $root '..\gbarecomp' }
if (-not $RecompUiRoot) { $RecompUiRoot = Join-Path $root '..\recomp-ui' }
if (-not $Rom) { $Rom = Join-Path $root 'variants\emerald\roms\emerald_usa.gba' }
$EngineRoot = (Resolve-Path -LiteralPath $EngineRoot).Path
$RecompUiRoot = (Resolve-Path -LiteralPath $RecompUiRoot).Path
$Rom = (Resolve-Path -LiteralPath $Rom).Path
if (-not $Bios) { $Bios = Join-Path $EngineRoot 'bios\gba_bios.bin' }
$Bios = (Resolve-Path -LiteralPath $Bios).Path
$out = Join-Path $root 'release-stage'
New-Item -ItemType Directory -Force $out | Out-Null
$artifacts = [System.Collections.Generic.List[string]]::new()

function Invoke-Native {
  param([string]$File, [string[]]$Arguments, [string]$What)
  & $File @Arguments
  if ($LASTEXITCODE -ne 0) { throw "$What failed ($LASTEXITCODE)" }
}

# Run a native tool, capturing stdout+stderr (PowerShell 5.1 turns redirected
# native stderr into terminating errors under ErrorActionPreference=Stop).
function Invoke-Captured {
  param([string]$File, [string[]]$Arguments, [string]$WorkingDirectory = (Get-Location).Path)
  $stdout = [IO.Path]::GetTempFileName()
  $stderr = [IO.Path]::GetTempFileName()
  try {
    $quoted = $Arguments | ForEach-Object { if ($_ -match '[\s"]') { '"' + $_.Replace('"', '\"') + '"' } else { $_ } }
    $p = Start-Process -FilePath $File -ArgumentList $quoted -WorkingDirectory $WorkingDirectory `
        -NoNewWindow -PassThru -RedirectStandardOutput $stdout -RedirectStandardError $stderr
    $null = $p.Handle   # .NET only records ExitCode once the handle is held
    $p.PriorityClass = 'BelowNormal'
    $p.WaitForExit()
    [pscustomobject]@{ ExitCode = $p.ExitCode
                       Output = ([IO.File]::ReadAllText($stdout) + [IO.File]::ReadAllText($stderr)) }
  } finally {
    [IO.File]::Delete($stdout)
    [IO.File]::Delete($stderr)
  }
}

function Assert-Pinned {
  param([string]$Checkout, [string]$Gitlink, [string[]]$InputPaths)
  $pin = ((git -C $root ls-tree HEAD $Gitlink) -split '\s+')[2]
  $head = (git -C $Checkout rev-parse HEAD).Trim()
  if ($pin -ne $head) { throw "$Gitlink pin is $pin but $Checkout is at $head" }
  $dirty = @(git -C $Checkout status --porcelain --untracked-files=no -- @InputPaths)
  if ($dirty.Count -and -not $AllowDirty) {
    throw "$Checkout has uncommitted build inputs:`n$($dirty -join "`n")"
  }
  Write-Host "pinned $Gitlink = $head"
}

# Reject anything that looks like a ROM, BIOS or save (name, size, header).
function Assert-NoPrivateAssets {
  param([string]$Dir)
  $bad = @(Get-ChildItem -LiteralPath $Dir -Recurse -File | Where-Object {
    $n = $_.Name.ToLowerInvariant()
    if ($n -match '\.(gba|agb|sav)$' -or $n -eq 'gba_bios.bin' -or
        ($_.Length -eq 16384 -and $n.EndsWith('.bin'))) { return $true }
    if ($_.Length -lt 0xC0) { return $false }
    $fs = [IO.File]::OpenRead($_.FullName)
    try { $h = New-Object byte[] 0xB0; [void]$fs.Read($h, 0, 0xB0) } finally { $fs.Dispose() }
    [Text.Encoding]::ASCII.GetString($h, 0xA0, 16) -eq 'POKEMON EMERBPEE'
  })
  if ($bad.Count) { throw "private assets in ${Dir}:`n$(($bad | ForEach-Object FullName) -join "`n")" }
}

function Get-WslPath([string]$Path) {
  $full = [IO.Path]::GetFullPath($Path)
  '/mnt/' + $full.Substring(0, 1).ToLowerInvariant() + $full.Substring(2).Replace('\', '/')
}

# ── 1. Pins ────────────────────────────────────────────────────────────────
if (-not $AllowDirty) {
  $selfDirty = @(git -C $root status --porcelain --untracked-files=no)
  if ($selfDirty.Count) { throw "EmeraldRecomp has uncommitted changes:`n$($selfDirty -join "`n")" }
}
Assert-Pinned -Checkout $EngineRoot -Gitlink 'gbarecomp' `
    -InputPaths @('src', 'tools', 'platform', 'cmake', 'CMakeLists.txt', 'external', 'bios')
Assert-Pinned -Checkout $RecompUiRoot -Gitlink 'recomp-ui' -InputPaths @('.')
$romSha = (Get-FileHash -Algorithm SHA1 -LiteralPath $Rom).Hash.ToLowerInvariant()
if ($romSha -ne 'f3ae088181bf583e55daf962a92bb46f4f1d07b7') { throw "ROM SHA-1 $romSha is not Emerald (USA)" }

# ── 2. Recompile ───────────────────────────────────────────────────────────
$toolBuild = Join-Path $root 'build-release-tool'
Invoke-Native "$MingwBin\cmake.exe" @('-S', $EngineRoot, '-B', $toolBuild, '-G', 'Ninja',
    "-DCMAKE_C_COMPILER=$MingwBin/cc.exe", "-DCMAKE_CXX_COMPILER=$MingwBin/c++.exe",
    "-DCMAKE_MAKE_PROGRAM=$MingwBin/ninja.exe", '-DCMAKE_BUILD_TYPE=Release',
    '-DGBARECOMP_BUILD_ORACLE=OFF') 'engine tool configure'
Invoke-Native "$MingwBin\cmake.exe" @('--build', $toolBuild, '--target', 'gba_recompile') 'gba_recompile build'
$regen = Invoke-Captured -File (Join-Path $toolBuild 'gba_recompile.exe') -WorkingDirectory (Join-Path $root 'variants\emerald') `
    -Arguments @('--rom', $Rom, '--config', 'game.toml', '--config', 'symbols/BPEE_symbols.toml',
                 '--config', 'symbols/BPEE_reviewed_seeds.toml', '--symbols', 'symbols/imported_symbols.tsv',
                 '--data-symbols', 'symbols/imported_data_symbols.tsv', '--out', 'generated',
                 '--max-functions', '65536')
$regen.Output | Out-File (Join-Path $toolBuild 'regen.log') -Encoding utf8
if ($regen.ExitCode -ne 0) { $regen.Output; throw "gba_recompile failed ($($regen.ExitCode))" }
[regex]::Matches($regen.Output, '==> discovered .*') | ForEach-Object { Write-Host $_.Value }

# ── 3. Windows ─────────────────────────────────────────────────────────────
if ($Platforms -contains 'windows') {
  $build = Join-Path $root 'build-release-windows'
  # Always (re)state the roots so a reused cache can never point elsewhere.
  Invoke-Native "$MingwBin\cmake.exe" @('-S', $root, '-B', $build, '-G', 'Ninja',
      "-DCMAKE_C_COMPILER=$MingwBin/cc.exe", "-DCMAKE_CXX_COMPILER=$MingwBin/c++.exe",
      "-DCMAKE_MAKE_PROGRAM=$MingwBin/ninja.exe", '-DCMAKE_BUILD_TYPE=Release',
      '-DCMAKE_CXX_FLAGS_RELEASE=-O1 -DNDEBUG', '-DGBARECOMP_BUILD_ORACLE=OFF',
      "-DGBARECOMP_ROOT=$($EngineRoot.Replace('\', '/'))",
      "-DRECOMP_UI_ROOT=$($RecompUiRoot.Replace('\', '/'))",
      "-DGBARECOMP_RUNTIME_UI_ROOT=$($RecompUiRoot.Replace('\', '/'))",
      '-DGBARECOMP_MINGW_PREFIX_UNIX=/c/msys64/mingw64',
      '-DSDL2_INCLUDE_DIR=C:/msys64/mingw64/include/SDL2',
      '-DSDL2_LIBRARY=C:/msys64/mingw64/lib/libSDL2.dll.a') 'windows configure'
  $p = Start-Process -FilePath "$MingwBin\cmake.exe" -NoNewWindow -PassThru `
      -ArgumentList @('--build', "`"$build`"", '--target', 'EmeraldRecomp', '-j', $Jobs)
  $null = $p.Handle
  $p.PriorityClass = 'BelowNormal'
  $p.WaitForExit()
  if ($p.ExitCode -ne 0) { throw "windows build failed ($($p.ExitCode))" }

  $exe = Join-Path $build 'EmeraldRecomp.exe'
  & "$MingwBin\strip.exe" $exe
  $stageName = "EmeraldRecomp-windows-x64-v$Version"
  $stage = Join-Path $out $stageName
  if (Test-Path -LiteralPath $stage) { Remove-Item -LiteralPath $stage -Recurse -Force }
  New-Item -ItemType Directory -Force $stage | Out-Null
  Copy-Item $exe $stage
  foreach ($d in @('SDL2.dll', 'libgcc_s_seh-1.dll', 'libstdc++-6.dll', 'libwinpthread-1.dll')) {
    Copy-Item (Join-Path $MingwBin $d) $stage
  }
  $assets = Join-Path $build 'assets'
  if (-not (Test-Path (Join-Path $assets 'img'))) { throw "recomp-ui launcher assets missing: $assets" }
  Copy-Item $assets -Destination $stage -Recurse
  # Checked-in mod catalog only, never a build dir's remembered selections.
  Copy-Item -LiteralPath (Join-Path $root 'mods\preloaded') -Destination (Join-Path $stage 'mods') -Recurse
  Copy-Item -LiteralPath (Join-Path $root 'LICENSE') -Destination $stage
  # Self-contained tcc overlay toolchain so toolchain-less players self-heal
  # overlay gaps (see gbarecomp/tools/fetch_tcc.ps1).
  & (Join-Path $EngineRoot 'tools\fetch_tcc.ps1') -Toolchain (Join-Path $stage 'overlay_toolchain') -EngineRoot $EngineRoot

  @"
# Pokemon Emerald - GBA static recompilation (Windows x64) v$Version

Release build: an optimized native port. Running ``EmeraldRecomp.exe`` opens the
launcher; pick your ROM (and, on first run, your GBA BIOS) and play.

Static recompilation turns the game's ARM7TDMI code into native C++ (via the
[gbarecomp](https://github.com/mstan/gbarecomp) framework); the rest of the GBA
(PPU, APU, DMA and timers) runs through the framework's runner core. The real
GBA BIOS is recompiled and executed; supply your own BIOS dump.

## How to run

1. Extract this folder (keep the four DLLs next to ``EmeraldRecomp.exe``).
2. Run ``EmeraldRecomp.exe``. On first launch it asks for:
   - your legally-obtained **Pokemon Emerald (USA)** ROM (``.gba``) - expected SHA-1
     ``f3ae088181bf583e55daf962a92bb46f4f1d07b7``
   - a **GBA BIOS** dump (``gba_bios.bin``).
   The picked paths are cached to ``rom.cfg`` / ``bios.cfg`` next to the exe;
   save data lands next to the exe.

The ROM and BIOS are **never** redistributed - supply your own dumps.

## Widescreen mod

Open **Mods**, enable **Overworld Widescreen (Experimental)**, and choose
**Fit to window**, **16:9**, **21:9** or **32:9**. Apply the selection and play.
The feature ships disabled and preserves native gameplay and save data. Fit
fills landscape and portrait windows with additional scenery and live NPCs;
the Start menu stays in reach at the right edge. Battles retain their native
3:2 view.

See the GitHub release notes for what changed in v$Version.
"@ | Out-File (Join-Path $stage 'README.md') -Encoding utf8

  Assert-NoPrivateAssets $stage

  # Headless smoke run from a scratch copy (the stage must stay pristine: a
  # run writes rom.cfg / bios.cfg / coverage files next to the exe).
  $smoke = Join-Path $env:TEMP "emeraldrecomp-smoke-$Version"
  if (Test-Path $smoke) { Remove-Item $smoke -Recurse -Force }
  Copy-Item $stage $smoke -Recurse
  try {
    $env:GBARECOMP_STRICT_STATIC = '1'
    $run = Invoke-Captured -File (Join-Path $smoke 'EmeraldRecomp.exe') -WorkingDirectory $smoke `
        -Arguments @('--no-launcher', '--no-window', '--frames', '1500', '--bios', $Bios, '--rom', $Rom,
                     '--save-path', (Join-Path $smoke 'smoke.sav'))
    if ($run.Output -notmatch 'self_heal_coverage=FULLY_STATIC') { $run.Output; throw 'windows smoke test is not FULLY_STATIC' }
    Write-Host 'windows smoke: FULLY_STATIC'
  } finally {
    $env:GBARECOMP_STRICT_STATIC = $null
    Remove-Item -LiteralPath $smoke -Recurse -Force -ErrorAction SilentlyContinue
  }

  $zip = Join-Path $out "$stageName.zip"
  if (Test-Path $zip) { Remove-Item -Force $zip }
  Add-Type -AssemblyName System.IO.Compression
  Add-Type -AssemblyName System.IO.Compression.FileSystem
  # ZIP entry names must use '/' (Compress-Archive keeps '\', which POSIX
  # extractors treat as literal filename characters). Write portably, then
  # read back and verify before anyone can publish it.
  $stageFull = [IO.Path]::GetFullPath($stage).TrimEnd('\') + '\'
  $files = @(Get-ChildItem -LiteralPath $stage -File -Recurse | Sort-Object FullName)
  $archive = [IO.Compression.ZipFile]::Open($zip, [IO.Compression.ZipArchiveMode]::Create)
  try {
    foreach ($f in $files) {
      $name = $f.FullName.Substring($stageFull.Length).Replace('\', '/')
      if ($name.StartsWith('/') -or $name -match '(^|/)\.\.(/|$)') { throw "Unsafe ZIP entry name: $name" }
      [IO.Compression.ZipFileExtensions]::CreateEntryFromFile($archive, $f.FullName, $name,
          [IO.Compression.CompressionLevel]::Optimal) | Out-Null
    }
  } finally { $archive.Dispose() }
  $archive = [IO.Compression.ZipFile]::OpenRead($zip)
  try {
    $badNames = @($archive.Entries | Where-Object { $_.FullName.Contains('\') -or $_.FullName.StartsWith('/') })
    if ($badNames.Count -or $archive.Entries.Count -ne $files.Count) { throw 'ZIP verification failed' }
  } finally { $archive.Dispose() }
  $artifacts.Add($zip)
  Write-Host "windows: $zip"
}

# ── 4. Linux AppImage ──────────────────────────────────────────────────────
if ($Platforms -contains 'linux') {
  # Private assets for the container smoke test: a scratch copy, mounted
  # read-only, deleted afterwards. They never enter the image.
  $private = Join-Path $env:TEMP "emeraldrecomp-private-$([guid]::NewGuid().ToString('N'))"
  New-Item -ItemType Directory $private | Out-Null
  try {
    Copy-Item -LiteralPath $Bios (Join-Path $private 'gba_bios.bin')
    Copy-Item -LiteralPath $Rom (Join-Path $private 'emerald_usa.gba')
    $env:MSYS_NO_PATHCONV = '1'
    Invoke-Native 'wsl' @('-e', 'bash', (Get-WslPath (Join-Path $root 'tools\linux\make_appimage.sh')),
        '--version', $Version, '--game', (Get-WslPath $root), '--engine', (Get-WslPath $EngineRoot),
        '--ui', (Get-WslPath $RecompUiRoot), '--out', (Get-WslPath $out),
        '--private', (Get-WslPath $private), '--jobs', "$Jobs") 'linux AppImage build'
  } finally { Remove-Item -LiteralPath $private -Recurse -Force -ErrorAction SilentlyContinue }
  $appimage = Join-Path $out "EmeraldRecomp-linux-x86_64-v$Version.AppImage"
  if (-not (Test-Path $appimage)) { throw "AppImage missing: $appimage" }
  $artifacts.Add($appimage)
  Write-Host "linux: $appimage"
}

# ── 5. Android ─────────────────────────────────────────────────────────────
if ($Platforms -contains 'android') {
  $parts = $Version.Split('.') | ForEach-Object { [int]$_ }
  $env:GBARECOMP_VERSION_NAME = $Version
  $env:GBARECOMP_VERSION_CODE = "$($parts[0] * 10000 + $parts[1] * 100 + $parts[2])"
  try {
    # No -PrivateRom / -PrivateBios: the template's verifyNoPrivateAssets
    # fails the build if a ROM or BIOS reaches the payload.
    & (Join-Path $EngineRoot 'platform\android\tools\build-apk.ps1') -GameAndroidDir (Join-Path $root 'android') `
        -EngineRoot $EngineRoot -RecompUiRoot $RecompUiRoot -Release -Abis 'arm64-v8a' -Jobs $Jobs
    if ($LASTEXITCODE -ne 0) { throw "android build failed ($LASTEXITCODE)" }
  } finally {
    Remove-Item Env:GBARECOMP_VERSION_NAME, Env:GBARECOMP_VERSION_CODE -ErrorAction SilentlyContinue
  }
  $built = Join-Path $root 'android\app\build\outputs\apk\release\app-release.apk'
  if (-not (Test-Path $built)) { throw "release APK missing: $built" }
  $apk = Join-Path $out "EmeraldRecomp-android-arm64-v$Version.apk"
  Copy-Item -LiteralPath $built -Destination $apk -Force

  # BYOR: list the APK and reject any ROM / BIOS / save.
  Add-Type -AssemblyName System.IO.Compression.FileSystem
  $zipApk = [IO.Compression.ZipFile]::OpenRead($apk)
  try {
    $bad = @($zipApk.Entries | Where-Object {
      $n = $_.Name.ToLowerInvariant()
      $n -match '\.(gba|agb|sav)$' -or $n -eq 'gba_bios.bin' -or $_.FullName -like 'assets/payload/roms/*' -or
      $_.FullName -like 'assets/payload/bios/*' -or ($_.Length -eq 16384 -and $n.EndsWith('.bin'))
    })
    if ($bad.Count) { throw "private assets in the APK: $(($bad | ForEach-Object FullName) -join ', ')" }
    $abis = @($zipApk.Entries | Where-Object { $_.FullName -like 'lib/*/libmain.so' } | ForEach-Object { $_.FullName.Split('/')[1] })
    if (($abis -join ',') -ne 'arm64-v8a') { throw "unexpected APK ABIs: $($abis -join ',')" }
  } finally { $zipApk.Dispose() }

  $buildTools = Get-ChildItem 'C:\Android\Sdk\build-tools' -Directory | Sort-Object { [version]$_.Name } | Select-Object -Last 1
  $badging = (Invoke-Captured -File (Join-Path $buildTools.FullName 'aapt2.exe') -Arguments @('dump', 'badging', $apk)).Output
  if ($badging -notmatch "versionName='$([regex]::Escape($Version))'") { throw "APK versionName is not $Version" }
  $java = if ($env:JAVA_HOME) { Join-Path $env:JAVA_HOME 'bin\java.exe' } else { (Get-Command java -ErrorAction Stop).Source }
  $signer = Invoke-Captured -File $java -Arguments @('-jar', (Join-Path $buildTools.FullName 'lib\apksigner.jar'),
      'verify', '--print-certs', $apk)
  # Must positively see a verified signer; an empty or errored run never passes.
  if ($signer.ExitCode -ne 0 -or $signer.Output -notmatch 'Signer #1 certificate DN:') {
    $signer.Output; throw 'apksigner verification failed'
  }
  Write-Host ([regex]::Match($signer.Output, 'Signer #1 certificate DN: .*').Value)
  if ($signer.Output -match 'CN=Android Debug') {
    Write-Warning 'The Android APK is signed with the local DEBUG key. Set GBARECOMP_KEYSTORE / _PASSWORD / GBARECOMP_KEY_ALIAS for a release key before publishing.'
  }
  $artifacts.Add($apk)
  Write-Host "android: $apk"
}

# ── Checksums ──────────────────────────────────────────────────────────────
# Every artifact of this version in release-stage (a partial -Platforms run
# keeps the others' checksums).
$all = @(Get-ChildItem -LiteralPath $out -File |
    Where-Object { $_.Name -match "^EmeraldRecomp-.*-v$([regex]::Escape($Version))\.(zip|AppImage|apk)$" } |
    Sort-Object Name | ForEach-Object FullName)
$sums = foreach ($a in $all) {
  "{0}  {1}" -f (Get-FileHash -Algorithm SHA256 -LiteralPath $a).Hash.ToLowerInvariant(), (Split-Path -Leaf $a)
}
[IO.File]::WriteAllLines((Join-Path $out 'SHA256SUMS.txt'), [string[]]$sums)
Write-Host "--- release-stage (v$Version) ---"
$artifacts | ForEach-Object { Get-Item $_ | Select-Object Name, Length } | Format-Table | Out-Host
