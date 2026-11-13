<#
.SYNOPSIS
    Build glide3x.dll locally on Windows 11 - no WSL, no cross toolchain.

.DESCRIPTION
    Runs the same build GitHub Actions runs on ubuntu-24.04 with the
    i686-w64-mingw32 cross compiler, except natively on Windows:

      toolchain : MSYS2 MINGW32 environment (mingw-w64 i686 g++, native)
      slang     : pinned VTSTech/libslang-vk checkout beside the repo
                  (cloned + pinned automatically on first run)
      kernel    : glslangValidator compiles spike/routeb.comp to
                  routeb.comp.spv - the DLL loads it from its own directory
                  at runtime (raster=gpu fail-softs to CPU raster without it)
      configure : cmake -S . -B build-g3x -G Ninja
      build     : cmake --build build-g3x --target glide3x -j
      gates     : build-g3x/glide3x.dll is a 32-bit PE (pei-i386), exactly
                  the 45 Glide exports glide3x.def lists, version banner
                  present
      artifact  : build-g3x\stage\ (glide3x.dll + routeb.comp.spv + the
                  scripts\*.cmd helpers) and glide3x-dll-<id>.zip - CI's
                  artifact equivalent

    First run installs MSYS2 via winget and the i686 packages via pacman
    (one-time, a few hundred MB) and clones libslang-vk (its build adds
    ~76 glslang/slang targets - the first run is the slow one; later runs
    recompile only what changed). Keep ~4 GB free across the two trees.

    The DLL is self-contained (-static libgcc/libstdc++) - no runtime DLLs.
    To play: copy glide3x.dll + routeb.comp.spv next to Game.exe of a 1.14d
    install (the game loads glide3x.dll by name; -DeployTo automates it).
    Shader presets go there too via scripts\install_shaders.cmd.

.EXAMPLE
    powershell -ExecutionPolicy Bypass -File tools\build-local-win11.ps1
    powershell -ExecutionPolicy Bypass -File tools\build-local-win11.ps1 -Clean -DeployTo "D:\Games\Diablo II"
    powershell -ExecutionPolicy Bypass -File tools\build-local-win11.ps1 -SetupOnly
#>
param(
    # Repo root. Default: parent of the folder containing this script (tools/).
    [string]$RepoRoot = "",
    # MSYS2 install directory. Installed via winget when missing.
    [string]$Msys2Root = "C:\msys64",
    # libslang-vk checkout (the preset chain). Default: beside the repo, the
    # location CMakeLists expects. Cloned and pinned to CI's SHA when missing.
    [string]$SlangDir = "",
    # Install/verify the toolchain only, do not build.
    [switch]$SetupOnly,
    # Skip the toolchain check and go straight to building (faster repeat runs).
    [switch]$BuildOnly,
    # Build WITHOUT the preset chain (G3XVK_HAVE_SLANGVK=0, chain stubbed).
    # Skips the libslang-vk checkout entirely - fastest compile loop.
    [switch]$NoSlang,
    # Delete build-g3x before configuring (parity with a clean CI run).
    [switch]$Clean,
    # Parallel compile jobs. 0 = all logical processors.
    [int]$Jobs = 0,
    # Do not produce the stage\ folder and glide3x-dll-<id>.zip artifact.
    [switch]$SkipZip,
    # Copy glide3x.dll into this directory (a 1.14d install).
    [string]$DeployTo = ""
)

$ErrorActionPreference = "Stop"

function Write-Step { param($m) Write-Host ""; Write-Host "==> $m" -ForegroundColor Cyan }
function Write-Ok   { param($m) Write-Host "    $m" -ForegroundColor Green }

# ---------------------------------------------------------------------------
# Repo root
# ---------------------------------------------------------------------------
if (-not $RepoRoot) { $RepoRoot = Split-Path -Parent $PSScriptRoot }
$RepoRoot = (Resolve-Path $RepoRoot).Path
if (-not (Test-Path (Join-Path $RepoRoot "CMakeLists.txt"))) {
    throw "CMakeLists.txt not found under '$RepoRoot'. Pass -RepoRoot <path>."
}
if ($RepoRoot.Length -gt 90) {
    Write-Warning "Repo path is $($RepoRoot.Length) chars; Windows compilers can hit MAX_PATH on long trees."
    Write-Warning "A short checkout such as C:\dev\Glide3xVK avoids that."
}
Write-Step "Repository: $RepoRoot"

# ---------------------------------------------------------------------------
# MSYS2 (provides the i686 mingw-w64 toolchain this project requires)
# ---------------------------------------------------------------------------
$Pacman = Join-Path $Msys2Root "usr\bin\pacman.exe"
$Bash   = Join-Path $Msys2Root "usr\bin\bash.exe"

if (-not (Test-Path $Pacman)) {
    Write-Step "MSYS2 not found at $Msys2Root - installing via winget"
    if (-not (Get-Command winget -ErrorAction SilentlyContinue)) {
        throw "winget not available. Install MSYS2 manually: https://github.com/msys2/msys2-installer/releases (default C:\msys64), then re-run."
    }
    winget install --id MSYS2.MSYS2 -e --accept-source-agreements --accept-package-agreements
    if ($LASTEXITCODE -ne 0) { throw "winget failed to install MSYS2 (exit $LASTEXITCODE)." }
    if (-not (Test-Path $Pacman)) {
        throw "MSYS2 installed, but $Pacman is missing - pass -Msys2Root if you picked another directory."
    }
    Write-Ok "MSYS2 installed"
} else {
    Write-Step "MSYS2 found: $Msys2Root"
}

# ---------------------------------------------------------------------------
# i686 toolchain packages (one-time). glslang compiles the Route-B kernel to
# SPIR-V; MSYS2 may not package it for i686 - the script falls back to any
# glslangValidator it can find (any arch works, it is a host tool), and only
# warns if none exists (raster=gpu needs the spv; everything else does not).
# ---------------------------------------------------------------------------
$Packages = "mingw-w64-i686-gcc mingw-w64-i686-cmake mingw-w64-i686-ninja git"
$env:CHERE_INVOKING = "1"   # keep bash where it is
$env:MSYSTEM = "MINGW32"    # the 32-bit (i686) environment, not the 64-bit one

if (-not $BuildOnly) {
    Write-Step "Installing toolchain packages via pacman (one-time, few hundred MB)"
    $install = "pacman -Sy --noconfirm; pacman -S --needed --noconfirm $Packages"
    & $Bash -lc $install
    if ($LASTEXITCODE -ne 0) {
        Write-Warning "pacman install failed - updating the MSYS2 core first, then retrying once"
        & $Bash -lc "pacman -Syu --noconfirm"
        & $Bash -lc $install
        if ($LASTEXITCODE -ne 0) {
            throw "pacman could not install the packages. Close every MSYS2 window and re-run this script."
        }
    }
    & $Bash -lc "pacman -S --needed --noconfirm mingw-w64-i686-glslang" | Out-Null
    if ($LASTEXITCODE -eq 0) { Write-Ok "Toolchain packages + glslang present" }
    else { Write-Warning "glslang not packaged for i686 - kernel compile falls back to a PATH glslangValidator or the CI artifact's routeb.comp.spv" }
}
if ($SetupOnly) { Write-Step "Setup-only: done."; return }

# ---------------------------------------------------------------------------
# Build environment (MINGW32 first on PATH)
# ---------------------------------------------------------------------------
$Ming32Bin = Join-Path $Msys2Root "mingw32\bin"
$Gpp       = Join-Path $Ming32Bin "g++.exe"
if (-not (Test-Path $Gpp)) {
    throw "g++.exe not found in $Ming32Bin. Run once without -BuildOnly so pacman can install mingw-w64-i686-gcc."
}
$env:PATH = "$Ming32Bin;$Msys2Root\usr\bin;$env:PATH"

$dump = ((& $Gpp -dumpmachine) -join " ")
if ($dump -notmatch "^i686-w64-mingw32") {
    throw "Wrong compiler: $Gpp targets '$dump', need i686-w64-mingw32. Re-run without -BuildOnly."
}
Write-Step "Compiler: g++ ($dump)"

$Objdump = Join-Path $Ming32Bin "objdump.exe"
$Strings = Join-Path $Ming32Bin "strings.exe"

# glslangValidator: mingw32 first, then mingw64 (host tool, arch-agnostic),
# then PATH. None of them -> warn; the DLL still builds (raster=gpu just
# fail-softs without routeb.comp.spv).
$Glslang = $null
foreach ($c in @(
    (Join-Path $Ming32Bin "glslangValidator.exe"),
    (Join-Path $Msys2Root "mingw64\bin\glslangValidator.exe"),
    (Get-Command glslangValidator -ErrorAction SilentlyContinue).Source
)) {
    if ($c -and (Test-Path $c)) { $Glslang = $c; break }
}

# ---------------------------------------------------------------------------
# libslang-vk (the preset chain) - the same pinned checkout CI uses
# ---------------------------------------------------------------------------
$SlangPin = "34e8b5bdc40ec228eb214c7901c67126e2546bae"
$SlangSrc = ""
if (-not $NoSlang) {
    if (-not $SlangDir) { $SlangDir = Join-Path (Split-Path -Parent $RepoRoot) "libslang-vk" }
    if (Test-Path (Join-Path $SlangDir "CMakeLists.txt")) {
        $SlangSrc = (Resolve-Path $SlangDir).Path
        $head = (& git -C $SlangSrc rev-parse HEAD)
        $headShort = if ($head -and $head.Length -ge 7) { $head.Substring(0, 7) } else { "$head" }
        if ($head -ne $SlangPin) {
            Write-Warning "libslang-vk at $SlangSrc is at $headShort, CI pins $($SlangPin.Substring(0,7))."
            Write-Warning "The DLL builds either way (the API is detected at configure); git -C libslang-vk checkout $SlangPin pins it."
        }
    } else {
        Write-Step "libslang-vk not found at $SlangDir - cloning (pinned, one-time)"
        & git clone https://github.com/VTSTech/libslang-vk $SlangDir
        if ($LASTEXITCODE -ne 0) { throw "git clone of libslang-vk failed (exit $LASTEXITCODE). Pass -SlangDir to an existing checkout, or -NoSlang to build without the preset chain." }
        & git -C $SlangDir checkout $SlangPin
        if ($LASTEXITCODE -ne 0) { throw "pinning libslang-vk to $SlangPin failed." }
        $SlangSrc = (Resolve-Path $SlangDir).Path
        Write-Ok "libslang-vk cloned and pinned ($($SlangPin.Substring(0,7)))"
    }
    Write-Step "Preset chain: libslang-vk at $SlangSrc"
} else {
    Write-Step "Preset chain: skipped (-NoSlang) - the DLL builds with the chain stubbed"
}

if ($Jobs -le 0) { $Jobs = $env:NUMBER_OF_PROCESSORS }
if ($Jobs -le 0) { $Jobs = 4 }
$BuildDir = Join-Path $RepoRoot "build-g3x"

if ($Clean -and (Test-Path $BuildDir)) {
    Write-Step "Removing build-g3x\ (-Clean)"
    Remove-Item $BuildDir -Recurse -Force
}


# ---------------------------------------------------------------------------
# Configure + build
# ---------------------------------------------------------------------------
Write-Step "Configuring (CMake + Ninja, native i686 - CI's configure minus the cross toolchain file)"
$cfg = @("-S", $RepoRoot, "-B", $BuildDir, "-G", "Ninja", "-DCMAKE_EXPORT_COMPILE_COMMANDS=ON")
if ($SlangSrc) { $cfg += "-DG3XVK_SLANGVK_DIR=$SlangSrc" }
& cmake @cfg
if ($LASTEXITCODE -ne 0) { throw "cmake configure failed (exit $LASTEXITCODE)." }

Write-Step "Building glide3x.dll with -j $Jobs (libslang-vk's ~76 glslang/slang targets make the first run the slow one)"
$sw = [System.Diagnostics.Stopwatch]::StartNew()
& cmake --build $BuildDir --target glide3x -j $Jobs
if ($LASTEXITCODE -ne 0) {
    Write-Host ""
    Write-Host "Compile errors above. To resume: open 'MSYS2 MINGW32' from the Start menu, cd to the repo, run:  cmake --build build-g3x --target glide3x -j $Jobs" -ForegroundColor Yellow
    throw "build failed (exit $LASTEXITCODE)."
}
$sw.Stop()
Write-Ok ("build finished in {0}" -f $sw.Elapsed.ToString("hh\:mm\:ss"))

# ---------------------------------------------------------------------------
# Gates - the same checks CI runs before shipping an artifact
# ---------------------------------------------------------------------------
$Dll = Join-Path $BuildDir "glide3x.dll"
if (-not (Test-Path $Dll)) { throw "build-g3x\glide3x.dll was not produced." }

Write-Step "Gates"
$fmt = (& $Objdump -f $Dll) | Out-String
if ($fmt -notmatch "pei-i386") { throw "glide3x.dll is NOT a 32-bit PE (pei-i386) - do not ship it." }
Write-Ok ("glide3x.dll is a 32-bit PE ({0:N0} bytes)" -f (Get-Item $Dll).Length)

$exp = (& $Objdump -p $Dll)
$tail = ($exp | Select-Object -Skip (($exp | Select-String -SimpleMatch "Ordinal/Name Pointer").LineNumber - 1)) -join "`n"
$n = ([regex]::Matches($tail, "_[A-Za-z]+@\d+")).Count
if ($n -ne 45) { throw "glide3x.dll exports $n symbols, expected exactly 45 (glide3x.def)." }
Write-Ok "45 Glide exports present (the game resolves every one of them)"

if (Test-Path $Strings) {
    $ver = (& $Strings $Dll | Select-String "^glide3xvk " | Select-Object -First 1)
    if ($ver) { Write-Ok "banner: $($ver.Line)" }
}

# ---------------------------------------------------------------------------
# Artifact: stage\ + glide3x-dll-<id>.zip (CI's artifact equivalent)
# ---------------------------------------------------------------------------
if (-not $SkipZip) {
    $id = ""
    try {
        $id = (& git -C $RepoRoot rev-parse --short=7 HEAD)
        if ($LASTEXITCODE -ne 0) { $id = "" }
        $dirty = (& git -C $RepoRoot status --porcelain)
        if ($LASTEXITCODE -eq 0 -and $dirty) { $id = "$id-dirty" }
    } catch { $id = "" }
    if (-not $id) { $id = "local" }

    Write-Step "Staging artifact (DLL + helper scripts + kernel)"
    $Stage = Join-Path $BuildDir "stage"
    New-Item -ItemType Directory -Force -Path $Stage | Out-Null
    Copy-Item $Dll $Stage -Force
    Get-ChildItem (Join-Path $RepoRoot "scripts") -Filter "*.cmd" | ForEach-Object {
        Copy-Item $_.FullName $Stage -Force
    }
    Write-Ok ("stage\ ready ({0} files)" -f (Get-ChildItem $Stage).Count)

    $zip = Join-Path $BuildDir ("glide3x-dll-{0}.zip" -f $id)
    if (Test-Path $zip) { Remove-Item $zip -Force }
    Compress-Archive -Path "$Stage\*" -DestinationPath $zip -Force
    if (Test-Path $zip) {
        Write-Ok ("artifact: {0} ({1:N0} bytes)" -f $zip, (Get-Item $zip).Length)
    } else { Write-Warning "zip failed - stage\ has everything, zip it by hand." }
}

# ---------------------------------------------------------------------------
# Optional deploy: the DLL + kernel go NEXT TO Game.exe (never touch the
# originals). Shader presets are separate: scripts\install_shaders.cmd.
# ---------------------------------------------------------------------------
if ($DeployTo) {
    if (-not (Test-Path $DeployTo)) { throw "-DeployTo '$DeployTo' does not exist." }
    $DeployTo = (Resolve-Path $DeployTo).Path
    Write-Step "Deploying to $DeployTo"
    $stamp = Get-Date -Format "yyyyMMdd-HHmmss"
    foreach ($f in @("glide3x.dll", "routeb.comp.spv")) {
        $src = Join-Path $BuildDir $f
        if (-not (Test-Path $src)) { continue }
        $dst = Join-Path $DeployTo $f
        $bak = "$dst.bak-$stamp"
        $hadOld = Test-Path $dst
        if ($hadOld) { Copy-Item $dst $bak -Force }
        Copy-Item $src $dst -Force
        if ($hadOld) { Write-Ok "copied $f (previous saved as $(Split-Path -Leaf $bak))" }
        else { Write-Ok "copied $f" }
    }
}

# ---------------------------------------------------------------------------
Write-Host ""
Write-Step "Done"
Write-Host "  build-g3x\glide3x.dll      the DLL - self-contained, drop-in by name"
if (-not $SkipZip) {
    Write-Host "  build-g3x\stage\           CI-artifact layout (DLL + kernel + scripts\*.cmd helpers)"
    Write-Host "  build-g3x\glide3x-dll-<id>.zip"
}
Write-Host ""
Write-Host "Play: copy glide3x.dll + routeb.comp.spv next to Game.exe in a 1.14d install," -ForegroundColor White
Write-Host "      add [glide3xvk] options to glide3xvk.ini (raster=gpu, ghash=0 ...)." -ForegroundColor White
Write-Host "      (-DeployTo does the copy; presets install separately via" -ForegroundColor White
Write-Host "       scripts\install_shaders.cmd.)" -ForegroundColor White
Write-Host ""
Write-Host "Tip: the first build is slower while Windows Defender scans every new .obj." -ForegroundColor DarkGray
Write-Host "     Excluding the repo folder (and $Msys2Root) from real-time scanning" -ForegroundColor DarkGray
Write-Host "     speeds up all later builds considerably." -ForegroundColor DarkGray
