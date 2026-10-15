# Downloads playit.exe (the Playit tunnel) from its official GitHub releases, for the release zip.
#   get-playit.ps1 -Out path\to\playit.exe
param([Parameter(Mandatory = $true)][string]$Out)
$ErrorActionPreference = "Stop"
$ProgressPreference = "SilentlyContinue"
[Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12
$headers = @{ "User-Agent" = "StrikersOnline-MakeRelease"; "Accept" = "application/vnd.github+json" }
$rel = Invoke-RestMethod -Uri "https://api.github.com/repos/playit-cloud/playit-agent/releases/latest" -Headers $headers -TimeoutSec 30
$exes = @($rel.assets | Where-Object { $_.name -like "*.exe" -and $_.name -like "*windows*" })
$pick = $exes | Where-Object { $_.name -like "*x86_64*" -and $_.name -like "*signed*" } | Select-Object -First 1
if (-not $pick) { $pick = $exes | Where-Object { $_.name -like "*x86_64*" } | Select-Object -First 1 }
if (-not $pick) { $pick = $exes | Select-Object -First 1 }
if (-not $pick) { throw "playit $($rel.tag_name) has no Windows .exe" }
$dir = Split-Path -Parent $Out
if ($dir -and -not (Test-Path -LiteralPath $dir)) { New-Item -ItemType Directory -Path $dir -Force | Out-Null }
Invoke-WebRequest -Uri $pick.browser_download_url -OutFile $Out -UseBasicParsing -Headers @{ "User-Agent" = "StrikersOnline-MakeRelease" }
Write-Host "  playit $($rel.tag_name) ($($pick.name))"
