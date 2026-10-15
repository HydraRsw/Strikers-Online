# Strikers Online updater.
#
# Fetches the newest release from GitHub and puts it over this folder, keeping your own files
# (strikers.ini, strikers-prefs.ini, logs). Your saves are not in this folder and are never touched.
#
#   updater.ps1                 ask, then update (what UPDATE.bat runs)
#   updater.ps1 -Yes            update without asking
#   updater.ps1 -WaitForGame    first wait for the game in this folder to close
#   updater.ps1 -Launch         start the game when done
#   updater.ps1 -CheckOnly -Out FILE
#                               only look: write current=, latest=, page= (or error=) to FILE,
#                               and the release notes to FILE's name with -notes before .txt
param(
    [string]$Repo = "HydraRsw/Strikers-Online",
    [switch]$CheckOnly,
    [string]$Out = "",
    [switch]$Yes,
    [switch]$WaitForGame,
    [switch]$Launch
)

$ErrorActionPreference = "Stop"
$ProgressPreference = "SilentlyContinue"   # Windows PowerShell downloads crawl with the progress bar on
[Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12

$dir = Split-Path -Parent $MyInvocation.MyCommand.Path
$versionFile = Join-Path $dir "version.txt"
$current = "unknown"
if (Test-Path -LiteralPath $versionFile) {
    $line = Get-Content -LiteralPath $versionFile -TotalCount 1
    if ($line) { $current = $line.Trim() }
}

# Files that are the player's own and must survive an update.
$keep = @("strikers.ini", "strikers-prefs.ini", "imgui.ini")

function Write-Atomic([string]$path, [string]$text) {
    $tmp = "$path.tmp"
    [IO.File]::WriteAllText($tmp, $text, (New-Object Text.UTF8Encoding($false)))
    Move-Item -LiteralPath $tmp -Destination $path -Force
}

function Get-Latest {
    $headers = @{ "User-Agent" = "StrikersOnline-Updater"; "Accept" = "application/vnd.github+json" }
    $rel = Invoke-RestMethod -Uri "https://api.github.com/repos/$Repo/releases/latest" -Headers $headers -TimeoutSec 20
    $asset = $rel.assets | Where-Object { $_.name -like "*windows*x86_64*.zip" } | Select-Object -First 1
    if (-not $asset) { $asset = $rel.assets | Where-Object { $_.name -like "*.zip" } | Select-Object -First 1 }
    return [pscustomobject]@{
        Tag   = [string]$rel.tag_name
        Page  = [string]$rel.html_url
        Notes = [string]$rel.body
        Url   = if ($asset) { [string]$asset.browser_download_url } else { "" }
        Size  = if ($asset) { [int64]$asset.size } else { 0 }
    }
}

# ---- Only looking (the game asks this in the background) ----
if ($CheckOnly) {
    if (-not $Out) { $Out = Join-Path $dir "update-check.txt" }
    $notesOut = [IO.Path]::ChangeExtension($Out, $null).TrimEnd('.') + "-notes.txt"
    try {
        $l = Get-Latest
        Write-Atomic $notesOut $l.Notes
        Write-Atomic $Out ("current=$current`nlatest=$($l.Tag)`npage=$($l.Page)`nurl=$($l.Url)`n")
    } catch {
        $msg = $_.Exception.Message -replace "[\r\n]+", " "
        Write-Atomic $Out ("current=$current`nerror=$msg`n")
    }
    exit 0
}

# ---- Updating ----
$Host.UI.RawUI.WindowTitle = "Strikers Online - Update"
Write-Host ""
Write-Host "  STRIKERS ONLINE - UPDATE" -ForegroundColor Green
Write-Host "  ========================" -ForegroundColor Green
Write-Host ""
Write-Host "  Your version:   $current"

try {
    $l = Get-Latest
} catch {
    Write-Host ""
    Write-Host "  Could not reach GitHub: $($_.Exception.Message)" -ForegroundColor Red
    Write-Host "  Check your internet connection and try again."
    Read-Host "  Press Enter to close" | Out-Null
    exit 1
}
Write-Host "  Newest version: $($l.Tag)"
Write-Host ""

if ($l.Tag -eq $current) {
    Write-Host "  You already have the newest version." -ForegroundColor Green
    if ($Launch) { Start-Process -FilePath (Join-Path $dir "strikers.exe") -WorkingDirectory $dir }
    if (-not $Yes) { Read-Host "  Press Enter to close" | Out-Null }
    exit 0
}
if (-not $l.Url) {
    Write-Host "  The newest release has no Windows download yet. Try again later." -ForegroundColor Yellow
    Read-Host "  Press Enter to close" | Out-Null
    exit 1
}

if (-not $Yes) {
    Write-Host "  Your strikers.ini, controls, sound settings and saves are kept."
    $answer = Read-Host "  Update now? (Y/n)"
    if ($answer -and $answer.Trim().ToLower().StartsWith("n")) { exit 0 }
}

# The game's files cannot be replaced while it runs.
function Get-GameProcesses {
    Get-Process -ErrorAction SilentlyContinue | Where-Object {
        try { $_.Path -and ([IO.Path]::GetDirectoryName($_.Path) -eq $dir) -and
              ($_.ProcessName -in @("strikers", "Mario Strikers Settings", "strikers_relay")) } catch { $false }
    }
}
$waited = 0
while (Get-GameProcesses) {
    if ($waited -eq 0) { Write-Host "  Waiting for the game to close..." -ForegroundColor Yellow }
    Start-Sleep -Seconds 1
    $waited++
    if ($waited -ge 60) {
        Write-Host "  The game (or Mario Strikers Settings, or the server) is still open." -ForegroundColor Red
        Write-Host "  Close it and run UPDATE.bat again."
        Read-Host "  Press Enter to close" | Out-Null
        exit 1
    }
}

$work = Join-Path $env:TEMP ("StrikersOnlineUpdate-" + [Guid]::NewGuid().ToString("N"))
New-Item -ItemType Directory -Path $work | Out-Null
try {
    $zip = Join-Path $work "update.zip"
    $mb = [Math]::Round($l.Size / 1MB)
    Write-Host "  Downloading $($l.Tag) ($mb MB)..."
    Invoke-WebRequest -Uri $l.Url -OutFile $zip -UseBasicParsing -Headers @{ "User-Agent" = "StrikersOnline-Updater" }

    Write-Host "  Unpacking..."
    $src = Join-Path $work "files"
    Expand-Archive -LiteralPath $zip -DestinationPath $src -Force
    # A zip holding one folder: its contents are the release.
    $top = @(Get-ChildItem -LiteralPath $src -Force)
    if ($top.Count -eq 1 -and $top[0].PSIsContainer) { $src = $top[0].FullName }
    if (-not (Test-Path -LiteralPath (Join-Path $src "strikers.exe"))) { throw "the download does not contain strikers.exe" }

    Write-Host "  Installing..."
    $count = 0
    Get-ChildItem -LiteralPath $src -Recurse -File -Force | ForEach-Object {
        $rel = $_.FullName.Substring($src.Length).TrimStart('\', '/')
        $dest = Join-Path $dir $rel
        if (($keep -contains $rel) -and (Test-Path -LiteralPath $dest)) { return }
        $destDir = Split-Path -Parent $dest
        if (-not (Test-Path -LiteralPath $destDir)) { New-Item -ItemType Directory -Path $destDir -Force | Out-Null }
        Copy-Item -LiteralPath $_.FullName -Destination $dest -Force
        $count++
    }
    # Files from a browser download are marked as coming from the internet; clear that.
    try { Get-ChildItem -LiteralPath $dir -Recurse -File | Unblock-File -ErrorAction SilentlyContinue } catch { }
    Write-Atomic $versionFile $l.Tag
    Write-Host ""
    Write-Host "  Updated to $($l.Tag) ($count files)." -ForegroundColor Green
} catch {
    Write-Host ""
    Write-Host "  The update failed: $($_.Exception.Message)" -ForegroundColor Red
    Write-Host "  Some files may not have been replaced. Run UPDATE.bat again, or download"
    Write-Host "  the release from $($l.Page)"
    Read-Host "  Press Enter to close" | Out-Null
    exit 1
} finally {
    Remove-Item -LiteralPath $work -Recurse -Force -ErrorAction SilentlyContinue
}

if ($Launch) {
    Write-Host "  Starting the game..."
    Start-Process -FilePath (Join-Path $dir "strikers.exe") -WorkingDirectory $dir
    Start-Sleep -Seconds 2
} elseif (-not $Yes) {
    Read-Host "  Press Enter to close" | Out-Null
}
exit 0
