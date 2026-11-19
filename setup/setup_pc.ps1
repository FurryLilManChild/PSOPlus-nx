<#
PSOPlus-nx: PC build tools setup (Windows).

Run from C:\PSOPlus-nx:
  powershell -ExecutionPolicy Bypass -File setup\setup_pc.ps1

What it does:
  1. Checks for git and turns this folder into a clone of github.com/FurryLilManChild/PSOPlus-nx (your
     assets\ folder is left alone; it is ignored by git).
  2. Downloads the pinned PC build tools into toolchain\ (Python 3.12 embeddable, ninja, decomp-toolkit,
     llvm-mingw clang, SDL2, Dear ImGui), each checked against the SHA-256 in setup\toolchain.json.
     Nothing is installed system-wide and no admin rights are needed.
  3. Prints a summary of what is ready. Everything is also written to setup.log.

It is safe to run again: finished tools are kept and only missing or changed ones are downloaded.
Based on sonic-riders-recomp's setup\setup.ps1 (MPL-2.0).
#>
param([switch]$SkipGit)

$ErrorActionPreference = "Stop"
$ProgressPreference = "SilentlyContinue"
[Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12

$Root = (Resolve-Path "$PSScriptRoot\..").Path
$LogFile = Join-Path $Root "setup.log"
$ToolsDir = Join-Path $Root "toolchain"
$Repo = "https://github.com/FurryLilManChild/PSOPlus-nx.git"

"" | Out-File -FilePath $LogFile -Encoding utf8
function Write-LogFile([string]$text) {
    for ($i = 0; $i -lt 20; $i++) {
        try { [IO.File]::AppendAllText($LogFile, $text + "`r`n", [Text.Encoding]::UTF8); return } catch { Start-Sleep -Milliseconds 100 }
    }
}
function Log([string]$msg, [string]$color = "") {
    Write-LogFile ("[{0}] {1}" -f (Get-Date -Format "HH:mm:ss"), $msg)
    if ($color) { Write-Host $msg -ForegroundColor $color } else { Write-Host $msg }
}
function Fail([string]$msg, [string]$hint = "") {
    Log ""; Log "ERROR: $msg" "Red"
    if ($hint) { Log $hint "Yellow" }
    Log "Send setup.log (in $Root) if you need help; it contains no game data." "Yellow"
    exit 1
}
function Native([string]$exe, [string[]]$a) {
    $old = $ErrorActionPreference; $ErrorActionPreference = "Continue"
    $out = (& $exe @a 2>&1 | ForEach-Object { "$_" }) -join "`n"
    $code = $LASTEXITCODE; $ErrorActionPreference = $old
    Write-LogFile "> $exe $($a -join ' ')`n$out"
    return @{ code = $code; out = $out }
}

Log "PSOPlus-nx PC setup" "Green"
Log "Folder: $Root"
Log ("Windows {0}, PowerShell {1}" -f [Environment]::OSVersion.Version, $PSVersionTable.PSVersion)
if ($Root -match "[^ -~]" -or $Root -match " ") { Fail "The folder path has spaces or non-English letters: $Root" "Use C:\PSOPlus-nx." }

# ---- 1. git and the clone ----
$commit = "(not checked)"
if (-not $SkipGit) {
    Log ""; Log "== git" "Cyan"
    $git = Get-Command git -ErrorAction SilentlyContinue
    if (-not $git) { Fail "git is not installed (or not on PATH)." "Install it with:  winget install --id Git.Git -e   then open a new PowerShell window and run this again." }
    Log ("  " + (Native "git" @("--version")).out)
    if (-not (Test-Path (Join-Path $Root ".git"))) {
        Log "  Making this folder a clone of $Repo (assets\ is ignored and kept)"
        foreach ($a in @(@("init", "-q"), @("remote", "add", "origin", $Repo))) {
            $r = Native "git" (@("-C", $Root) + $a); if ($r.code -ne 0) { Fail "git $($a -join ' ') failed: $($r.out)" }
        }
    }
    $r = Native "git" @("-C", $Root, "fetch", "-q", "origin", "main")
    if ($r.code -ne 0) { Fail "git fetch failed: $($r.out)" "Check the internet connection and that you are signed in to GitHub (the repo is private): run  git fetch  in this folder once to sign in." }
    $r = Native "git" @("-C", $Root, "checkout", "-q", "-f", "-B", "main", "origin/main")
    if ($r.code -ne 0) { Fail "git checkout failed: $($r.out)" }
    Native "git" @("-C", $Root, "config", "core.hooksPath", "tools/git-hooks") | Out-Null
    $commit = (Native "git" @("-C", $Root, "log", "-1", "--format=%h %s")).out
    Log "  Up to date with GitHub: $commit"
}

# ---- 2. tools ----
$tc = Get-Content (Join-Path $Root "setup\toolchain.json") -Raw | ConvertFrom-Json
$Cache = Join-Path $ToolsDir "_downloads"
New-Item -ItemType Directory -Force -Path $Cache | Out-Null

function Download([string]$url, [string]$sha256) {
    $name = [IO.Path]::GetFileName(([Uri]$url).AbsolutePath)
    $file = Join-Path $Cache $name
    if ((Test-Path $file) -and ((Get-FileHash $file -Algorithm SHA256).Hash.ToLower() -eq $sha256)) { return $file }
    Log "  downloading $name"
    $tmp = "$file.part"
    if (Test-Path $tmp) { Remove-Item $tmp -Force }
    $curl = Get-Command curl.exe -ErrorAction SilentlyContinue
    if ($curl) {
        for ($try = 1; $try -le 4; $try++) {
            $r = Native $curl.Source @("-fsSL", "--retry", "3", "-o", $tmp, $url)
            if ($r.code -eq 0) { break }
            Log "  interrupted (curl exit $($r.code)); trying again ($($try + 1) of 4)" "Yellow"; Start-Sleep -Seconds (3 * $try)
        }
        if ($r.code -ne 0) { Fail "Download failed: $url" "Check the internet connection and run this again; finished downloads are kept." }
    } else {
        try { Invoke-WebRequest -Uri $url -OutFile $tmp -UseBasicParsing } catch { Fail "Download failed: $url ($($_.Exception.Message))" }
    }
    $got = (Get-FileHash $tmp -Algorithm SHA256).Hash.ToLower()
    if ($got -ne $sha256) { Remove-Item $tmp -Force; Fail "Checksum mismatch for $name (got $got)." "Run this again; if it repeats, send setup.log." }
    Move-Item $tmp $file -Force
    return $file
}

Add-Type -AssemblyName System.IO.Compression.FileSystem
function Ensure-Tool($spec, [string]$label) {
    $dir = Join-Path $ToolsDir $spec.dir
    $mark = Join-Path $dir ".setup-ok"
    if ((Test-Path $mark) -and ((Get-Content $mark -Raw).Trim() -eq $spec.sha256)) { Log "  $label ready"; return $dir }
    $file = Download $spec.url $spec.sha256
    if (Test-Path $dir) { Remove-Item $dir -Recurse -Force }
    if ($spec.kind -eq "file") {
        New-Item -ItemType Directory -Force -Path $dir | Out-Null
        Copy-Item $file (Join-Path $dir $spec.name) | Out-Null
    } else {
        Log "  extracting $label"
        $stage = "$dir.extract"
        if (Test-Path $stage) { Remove-Item $stage -Recurse -Force }
        try { [IO.Compression.ZipFile]::ExtractToDirectory($file, $stage) } catch { Fail "Could not extract $file ($($_.Exception.Message))" }
        $src = $stage
        if ($spec.strip) { $src = Join-Path $stage $spec.strip }
        Move-Item $src $dir | Out-Null
        if (Test-Path $stage) { Remove-Item $stage -Recurse -Force }
    }
    Set-Content -Path $mark -Value $spec.sha256
    Log "  $label ready"
    return $dir
}

Log ""; Log "== build tools (into $ToolsDir)" "Cyan"
$pyDir   = Ensure-Tool $tc.python "Python 3.12"
Get-ChildItem $pyDir -Filter "python*._pth" | Remove-Item -Force   # let scripts import from their own folder
$zstdDir = Ensure-Tool $tc.zstandard "zstandard (for .rvz images)"
$ninjaDir = Ensure-Tool $tc.ninja "ninja"
$dtkDir  = Ensure-Tool $tc.dtk "decomp-toolkit 1.8.4"
Copy-Item (Join-Path $pyDir "vcruntime140.dll") $dtkDir -Force   # dtk.exe needs the Visual C++ runtime
$imguiDir = Ensure-Tool $tc.imgui "Dear ImGui 1.92.9b"
$llvmDir = Ensure-Tool $tc.llvm_mingw "llvm-mingw (clang for Windows)"
$sdlDir  = Ensure-Tool $tc.sdl2 "SDL2 2.32.10"

# ---- 3. summary ----
Log ""; Log "== check" "Cyan"
$checks = @(
    @("python", (Join-Path $pyDir "python.exe"), @("--version")),
    @("ninja", (Join-Path $ninjaDir "ninja.exe"), @("--version")),
    @("dtk", (Join-Path $dtkDir "dtk.exe"), @("--version")),
    @("clang", (Join-Path $llvmDir "bin\clang.exe"), @("--version"))
)
$bad = 0
foreach ($c in $checks) {
    $r = Native $c[1] $c[2]
    $first = ($r.out -split "`n")[0]
    if ($r.code -eq 0) { Log ("  {0,-7} OK   {1}" -f $c[0], $first) "Green" } else { Log ("  {0,-7} FAIL {1}" -f $c[0], $first) "Red"; $bad++ }
}
$sdlInc = Get-ChildItem $sdlDir -Recurse -Filter "SDL.h" | Select-Object -First 1
if ($sdlInc) { Log "  SDL2    OK   $($sdlInc.Directory.FullName)" "Green" } else { Log "  SDL2    FAIL (no SDL.h)" "Red"; $bad++ }
$disc = Get-ChildItem (Join-Path $Root "assets") -File -ErrorAction SilentlyContinue | Where-Object { $_.Extension -in ".rvz", ".iso" } | Select-Object -First 1
if ($disc) { Log "  disc    OK   $($disc.Name)" "Green" } else { Log "  disc    MISSING (put the .rvz or .iso in assets\)" "Yellow" }
Log "  repo    $commit"
Log ""
if ($bad) { Fail "$bad tool(s) failed the check." }
Log "SETUP RESULT: PASS. The PC build tools are ready. Send setup.log or just tell Claude it passed." "Green"
