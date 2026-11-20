<#
PSOPlus-nx: build the PC version on the owner's PC (gate 1: only the latest push is ever built).

Run from C:\PSOPlus-nx after setup\setup_pc.ps1 has passed once:
  powershell -ExecutionPolicy Bypass -File setup\build_pc.ps1

Steps (each printed as "step N of 7"):
  1. git: fetch GitHub's main and fast-forward to it; print the commit being built. Nothing is built if this
     folder has local changes to tracked files or cannot fast-forward.
  2. Disc: the first .rvz/.iso in assets\ becomes baserom\pso.iso once (converted from .rvz; SHA-1 checked).
  3. PsoV3.dol and movie.rel copied from the disc into baserom\ (SHA-1 checked).
  4. decomp-toolkit analysis (only when config\GPOE8P\symbols.txt is missing or the seed or config changed).
  5. configure (PC, llvm-mingw clang, SDL2).
  6. ninja: recompile and build; its own [n/N] counter shows progress.
  7. Summary: the commit, the program, its size.
Everything is also written to build_pc.log. The program is build\host-mingw\psoplus.exe; start it with
Start-Process and this folder as the working directory (then its log goes to psoplus.log here).
#>
$ErrorActionPreference = "Stop"
$ProgressPreference = "SilentlyContinue"

$Root = (Resolve-Path "$PSScriptRoot\..").Path
Set-Location $Root
$LogFile = Join-Path $Root "build_pc.log"
$Tools = Join-Path $Root "toolchain"
$IsoSha1 = "0ff155311f4cc57951c03baeb1a8cc15de5fe1cd"
$DolSha1 = "0d85744b51d7379efb025a943d0cd5aaae2191a3"
$RelSha1 = "767332daf33120d9497dc6426ed644c46bbf3ff3"   # movie.rel, the Sofdec movie player module

"" | Out-File -FilePath $LogFile -Encoding utf8
function Write-LogFile([string]$text) { [IO.File]::AppendAllText($LogFile, $text + "`r`n", [Text.Encoding]::UTF8) }
function Log([string]$msg, [string]$color = "") {
    Write-LogFile ("[{0}] {1}" -f (Get-Date -Format "HH:mm:ss"), $msg)
    if ($color) { Write-Host $msg -ForegroundColor $color } else { Write-Host $msg }
}
function Fail([string]$msg) {
    Log ""; Log "BUILD RESULT: FAIL. $msg" "Red"
    Log "Nothing new was built. Send build_pc.log (in $Root) to Claude." "Yellow"
    exit 1
}
function Step([int]$n, [string]$what) { Log ""; Log "== step $n of 7: $what" "Cyan" }
function Run([string]$what, [string]$exe, [string[]]$a) {
    Write-LogFile "> $exe $($a -join ' ')"
    $old = $ErrorActionPreference; $ErrorActionPreference = "Continue"
    & $exe @a 2>&1 | ForEach-Object { $l = "$_"; Write-LogFile $l; Write-Host "  $l" }
    $code = $LASTEXITCODE; $ErrorActionPreference = $old
    if ($code -ne 0) { Fail "$what failed (exit code $code)." }
}
function Capture([string]$exe, [string[]]$a) {
    $old = $ErrorActionPreference; $ErrorActionPreference = "Continue"
    $out = (& $exe @a 2>&1 | ForEach-Object { "$_" }) -join "`n"
    $code = $LASTEXITCODE; $ErrorActionPreference = $old
    Write-LogFile "> $exe $($a -join ' ')`n$out"
    return @{ code = $code; out = $out.Trim() }
}

$py = Join-Path $Tools "python\python.exe"
$ninja = Join-Path $Tools "ninja\ninja.exe"
$dtk = Join-Path $Tools "dtk\dtk.exe"
foreach ($t in $py, $ninja, $dtk, (Join-Path $Tools "llvm-mingw\bin\clang.exe")) {
    if (-not (Test-Path $t)) { Fail "Missing $t. Run setup\setup_pc.ps1 first." }
}
$env:PYTHONNOUSERSITE = "1"

# ---- 1. latest push only ----
Step 1 "get the latest push from GitHub"
$r = Capture "git" @("fetch", "-q", "origin", "main"); if ($r.code -ne 0) { Fail "git fetch failed: $($r.out)" }
$r = Capture "git" @("status", "--porcelain", "--untracked-files=no")
if ($r.out) { Fail "This folder has local changes to tracked files:`n$($r.out)`nTell Claude; nothing was changed." }
$r = Capture "git" @("merge", "--ff-only", "-q", "origin/main"); if ($r.code -ne 0) { Fail "Cannot fast-forward to GitHub's main: $($r.out)" }
$head = (Capture "git" @("rev-parse", "--short=8", "HEAD")).out
$remote = (Capture "git" @("rev-parse", "--short=8", "origin/main")).out
if ($head -ne $remote) { Fail "HEAD $head is not GitHub's main $remote." }
$subject = (Capture "git" @("log", "-1", "--format=%s")).out
Log "  Building commit $head ($subject), the latest push." "Green"

# ---- 2. disc image ----
Step 2 "disc image"
New-Item -ItemType Directory -Force -Path (Join-Path $Root "baserom") | Out-Null
$iso = Join-Path $Root "baserom\pso.iso"
if (-not (Test-Path $iso)) {
    $src = Get-ChildItem (Join-Path $Root "assets") -File -ErrorAction SilentlyContinue | Where-Object { $_.Extension -in ".rvz", ".iso" } | Select-Object -First 1
    if (-not $src) { Fail "No .rvz or .iso in assets\." }
    if ($src.Extension -eq ".iso") {
        Log "  Copying $($src.Name) to baserom\pso.iso"
        Copy-Item $src.FullName $iso
    } else {
        Log "  Converting $($src.Name) to baserom\pso.iso (about a minute)"
        Run "RVZ conversion" $py @("tools\disc\rvz2iso.py", $src.FullName, $iso, "--sha1", $IsoSha1, "--zstd-dir", (Join-Path $Tools "zstandard"))
    }
} else { Log "  baserom\pso.iso is already there" }

# ---- 3. PsoV3.dol ----
Step 3 "PsoV3.dol and movie.rel from the disc"
$dol = Join-Path $Root "baserom\PsoV3.dol"
$ok = (Test-Path $dol) -and ((Get-FileHash $dol -Algorithm SHA1).Hash.ToLower() -eq $DolSha1)
if (-not $ok) { Run "PsoV3.dol extraction" $py @("tools\disc\extract_file.py", $iso, "PsoV3.dol", $dol, "--sha1", $DolSha1) }
else { Log "  baserom\PsoV3.dol is already there (SHA-1 checked)" }
$rel = Join-Path $Root "baserom\movie.rel"
$ok = (Test-Path $rel) -and ((Get-FileHash $rel -Algorithm SHA1).Hash.ToLower() -eq $RelSha1)
if (-not $ok) { Run "movie.rel extraction" $py @("tools\disc\extract_file.py", $iso, "movie.rel", $rel, "--sha1", $RelSha1) }
else { Log "  baserom\movie.rel is already there (SHA-1 checked)" }

# ---- 4. decomp-toolkit ----
Step 4 "decomp-toolkit analysis"
$sym = Join-Path $Root "config\GPOE8P\symbols.txt"
$seed = Join-Path $Root "config\GPOE8P\symbols_seed.txt"
$mark = Join-Path $Root "build\GPOE8P\.seed-sha1"
$seedHash = (Get-FileHash $seed -Algorithm SHA1).Hash + (Get-FileHash (Join-Path $Root "config\GPOE8P\config.yml") -Algorithm SHA1).Hash
$need = -not (Test-Path $sym) -or -not (Test-Path $mark) -or ((Get-Content $mark -Raw).Trim() -ne $seedHash)
if ($need) {
    Copy-Item $seed $sym -Force
    [IO.File]::WriteAllText((Join-Path $Root "config\GPOE8P\splits.txt"), "")
    New-Item -ItemType Directory -Force -Path (Join-Path $Root "config\GPOE8P\movie") | Out-Null
    foreach ($f in "symbols.txt", "splits.txt") { [IO.File]::WriteAllText((Join-Path $Root "config\GPOE8P\movie\$f"), "") }
    Run "decomp-toolkit" $dtk @("dol", "split", "config\GPOE8P\config.yml", "build\GPOE8P")
    Set-Content -Path $mark -Value $seedHash
} else { Log "  analysis is up to date" }

# ---- 5. configure ----
Step 5 "configure (PC)"
Run "configure" $py @("configure.py", "--target", "host", "--toolchain", "mingw", "--llvm-mingw", (Join-Path $Tools "llvm-mingw"),
    "--sdl", (Join-Path $Tools "sdl2"), "--imgui", (Join-Path $Tools "imgui"))

# ---- 6. build ----
Step 6 "build (the first build compiles about 100 files; later builds only what changed)"
Run "ninja" $ninja @()

# ---- 7. summary ----
Step 7 "summary"
$exe = Join-Path $Root "build\host-mingw\psoplus.exe"
if (-not (Test-Path $exe)) { Fail "ninja finished but build\host-mingw\psoplus.exe is missing." }
$size = [math]::Round((Get-Item $exe).Length / 1MB, 1)
Log "  commit   $head"
Log "  program  build\host-mingw\psoplus.exe ($size MB)"
Log ""
Log "BUILD RESULT: PASS (commit $head). Run it with:  Start-Process .\build\host-mingw\psoplus.exe -WorkingDirectory $Root" "Green"
