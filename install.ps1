# install.ps1 — TapeXPlayer Windows Installer
# Usage:
#   iwr -useb https://raw.githubusercontent.com/ffbsoffa/TapeXPlayer/refs/heads/stable/install.ps1 | iex
#
# Flags (pass via $env: variables before piping, or use file mode):
#   $env:TAPEX_QUIET = "1"   — non-interactive (auto-confirm all prompts)
#   $env:TAPEX_FORCE = "1"   — force reinstall even if already installed
#   $env:TAPEX_PORTABLE = "1"— install to %LOCALAPPDATA% (no admin required)
#
# Or run as file:
#   powershell -ExecutionPolicy Bypass -File install.ps1 [-Quiet] [-Force] [-Portable]

[CmdletBinding()]
param(
    [switch]$Quiet,
    [switch]$Force,
    [switch]$Portable
)

# ── Configuration ─────────────────────────────────────────────────────────────
$GITHUB_REPO  = "ffbsoffa/TapeXPlayer"
$APP_NAME     = "TapeXPlayer"
$MIN_WIN_BUILD = 18362   # Windows 10 version 1903

# ── Merge env-var flags ───────────────────────────────────────────────────────
if ($env:TAPEX_QUIET   -eq "1") { $Quiet    = $true }
if ($env:TAPEX_FORCE   -eq "1") { $Force    = $true }
if ($env:TAPEX_PORTABLE -eq "1") { $Portable = $true }

# ── Error handling ────────────────────────────────────────────────────────────
$ErrorActionPreference = "Stop"

# Force TLS 1.2 for all .NET HTTP calls (GitHub blocks TLS 1.0/1.1, which is
# the default on Windows PowerShell 5.1 with older .NET Framework)
[Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12

function Write-Header {
    Write-Host ""
    Write-Host "================================================================" -ForegroundColor Cyan
    Write-Host "           TapeXPlayer Installer for Windows" -ForegroundColor Cyan
    Write-Host "================================================================" -ForegroundColor Cyan
    Write-Host ""
}

function Write-Step($n, $msg) { Write-Host "$n   $msg" -ForegroundColor White }
function Write-OK($msg)    { Write-Host "   OK  $msg" -ForegroundColor Green }
function Write-Warn($msg)  { Write-Host "   !!  $msg" -ForegroundColor Yellow }
function Write-Fail($msg)  {
    Write-Host ""
    Write-Host "   ERROR: $msg" -ForegroundColor Red
    Write-Host ""
    if (-not $Quiet) { Read-Host "Press Enter to exit" }
    exit 1
}

function Ask-YesNo($prompt) {
    if ($Quiet) { return $true }
    $r = Read-Host "   $prompt (y/n)"
    return $r -match '^[Yy]'
}

# ── Windows version check ─────────────────────────────────────────────────────
function Check-WindowsVersion {
    $build = [System.Environment]::OSVersion.Version.Build
    if ($build -lt $MIN_WIN_BUILD) {
        Write-Fail "Windows 10 (build $MIN_WIN_BUILD / version 1903) or later required. Found build: $build"
    }
    $ver = [System.Environment]::OSVersion.Version
    Write-Host "   Windows $($ver.Major).$($ver.Minor) (build $build)" -ForegroundColor Gray
}

# ── PowerShell version check ──────────────────────────────────────────────────
function Check-PSVersion {
    if ($PSVersionTable.PSVersion.Major -lt 5) {
        Write-Fail "PowerShell 5.1 or later required. Found: $($PSVersionTable.PSVersion)"
    }
}

# ── Fetch latest release from GitHub ─────────────────────────────────────────
# Uses /releases/latest first (non-prerelease), falls back to /releases?per_page=5
# (which includes pre-releases) — mirrors the two-stage search in install.sh.
function _Find-WinAsset($releaseObj) {
    # Stage 1: explicit -win.zip suffix
    $a = $releaseObj.assets | Where-Object { $_.name -match '-win\.zip$' } |
         Select-Object -First 1
    if ($a) { return $a }
    # Stage 2: any .zip that isn't mac/linux
    $a = $releaseObj.assets | Where-Object {
        $_.name -match '\.zip$' -and $_.name -notmatch '-mac|-linux'
    } | Select-Object -First 1
    return $a
}

function Get-DownloadUrl {
    $apiLatest = "https://api.github.com/repos/$GITHUB_REPO/releases/latest"
    $apiList   = "https://api.github.com/repos/$GITHUB_REPO/releases?per_page=5"

    # GitHub API requires User-Agent; Accept header selects stable JSON schema
    $ghHeaders = @{
        'Accept'     = 'application/vnd.github.v3+json'
        'User-Agent' = 'TapeXPlayer-Installer'
    }

    # ── Stage 1: /releases/latest (only non-prerelease) ──────────────────────
    $response = $null
    $asset    = $null
    try {
        $response = Invoke-RestMethod -Uri $apiLatest -UseBasicParsing -Headers $ghHeaders
        $asset    = _Find-WinAsset $response
    } catch {
        Write-Warn "Could not reach /releases/latest — trying recent releases list..."
    }

    # ── Stage 2: /releases?per_page=5 (includes pre-releases) ────────────────
    if (-not $asset) {
        if ($response) {
            Write-Warn "No Windows ZIP in latest release — searching recent releases..."
        }
        try {
            $releases = Invoke-RestMethod -Uri $apiList -UseBasicParsing -Headers $ghHeaders
        } catch {
            Write-Fail "Cannot reach GitHub API. Check your internet connection.`n$_"
        }
        foreach ($rel in $releases) {
            $candidate = _Find-WinAsset $rel
            if ($candidate) {
                $asset    = $candidate
                $response = $rel
                break
            }
        }
    }

    if (-not $asset) {
        Write-Fail "No Windows ZIP found in recent GitHub releases.`nCheck: https://github.com/$GITHUB_REPO/releases"
    }

    return @{
        Url     = $asset.browser_download_url
        Name    = $asset.name
        TagName = $response.tag_name
    }
}

# ── SHA256 verification (optional) ───────────────────────────────────────────
function Verify-SHA256($filePath, $shaUrl) {
    try {
        # Use Invoke-WebRequest (not Invoke-RestMethod) for plain-text .sha256 files:
        # Invoke-RestMethod tries to parse the body as JSON/XML and can mangle plain text.
        $resp     = Invoke-WebRequest -Uri $shaUrl -UseBasicParsing -ErrorAction Stop
        $expected = ($resp.Content.Trim() -split '\s+')[0]
    } catch { return }   # no checksum file — skip silently

    if (-not $expected) { return }

    $actual = (Get-FileHash -Algorithm SHA256 -Path $filePath).Hash.ToLower()
    if ($actual -ne $expected.ToLower()) {
        Write-Fail "SHA256 mismatch!`n   Expected: $expected`n   Actual:   $actual`n   The download may be corrupted."
    }
    Write-OK "SHA256 verified"
}

# ── Validate that all critical DLLs are present ───────────────────────────────
function Validate-Bundle($dir) {
    # Glob patterns — at least one match required
    $globs = @(
        "avcodec-*.dll",
        "avformat-*.dll",
        "avutil-*.dll",
        "swscale-*.dll",
        "swresample-*.dll"
    )
    foreach ($glob in $globs) {
        if (-not (Get-Item "$dir\$glob" -ErrorAction SilentlyContinue)) {
            Write-Fail "Broken archive: $glob not found.`n   The download may be incomplete or corrupted."
        }
    }

    # Exact names required
    $required = @(
        "SDL2.dll",
        "libstdc++-6.dll",
        "libgcc_s_seh-1.dll",
        "libwinpthread-1.dll"
    )
    $missing = @()
    foreach ($dll in $required) {
        if (-not (Test-Path "$dir\$dll")) { $missing += $dll }
    }
    if ($missing.Count -gt 0) {
        Write-Fail "Broken archive: missing required DLLs: $($missing -join ', ')`n   The download may be incomplete or corrupted."
    }

    $dllCount = @(Get-Item "$dir\*.dll" -ErrorAction SilentlyContinue).Count
    Write-OK "Bundle integrity OK ($dllCount DLLs, all critical deps present)"
}

# ── Elevation helper ──────────────────────────────────────────────────────────
function Test-IsAdmin {
    $identity  = [Security.Principal.WindowsIdentity]::GetCurrent()
    $principal = New-Object Security.Principal.WindowsPrincipal($identity)
    return $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
}

function Request-Elevation($scriptPath) {
    # Relaunch this script with admin rights
    $args = "-ExecutionPolicy Bypass -File `"$scriptPath`""
    if ($Quiet)    { $args += " -Quiet" }
    if ($Force)    { $args += " -Force" }
    if ($Portable) { $args += " -Portable" }
    Start-Process powershell -Verb RunAs -ArgumentList $args -Wait
    exit 0
}

# ── Create a .lnk shortcut ────────────────────────────────────────────────────
function New-Shortcut($shortcutPath, $targetPath, $description) {
    try {
        $wsh = New-Object -ComObject WScript.Shell
        $sc  = $wsh.CreateShortcut($shortcutPath)
        $sc.TargetPath       = $targetPath
        $sc.Description      = $description
        $sc.WorkingDirectory = Split-Path $targetPath
        $sc.Save()
    } catch {
        Write-Warn "Could not create shortcut: $_"
    }
}

# ── Register in Apps & Features ───────────────────────────────────────────────
function Register-UninstallEntry($installPath, $version) {
    $regRoot = if (Test-IsAdmin) { "HKLM:" } else { "HKCU:" }
    $regPath = "$regRoot\SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall\$APP_NAME"
    try {
        New-Item -Path $regPath -Force | Out-Null
        Set-ItemProperty $regPath "DisplayName"      "$APP_NAME"
        Set-ItemProperty $regPath "DisplayVersion"   $version
        Set-ItemProperty $regPath "Publisher"        "TapeXPlayer Team"
        Set-ItemProperty $regPath "InstallLocation"  $installPath
        Set-ItemProperty $regPath "UninstallString"  "powershell -ExecutionPolicy Bypass -File `"$installPath\uninstall.ps1`""
        Set-ItemProperty $regPath "DisplayIcon"      "$installPath\$APP_NAME.exe"
        Set-ItemProperty $regPath "URLInfoAbout"     "https://github.com/$GITHUB_REPO"
        Set-ItemProperty $regPath "NoModify"         1 -Type DWord
        Set-ItemProperty $regPath "NoRepair"         1 -Type DWord
    } catch {
        Write-Warn "Could not register in Apps & Features: $_"
    }
}

# ═════════════════════════════════════════════════════════════════════════════
# MAIN
# ═════════════════════════════════════════════════════════════════════════════

Write-Header
Check-PSVersion
Check-WindowsVersion

# Determine install directory
if ($Portable -or -not (Test-IsAdmin)) {
    $installDir = Join-Path $env:LOCALAPPDATA $APP_NAME
    $needsAdmin = $false
    if (-not $Portable) {
        Write-Warn "Running without administrator rights."
        Write-Host "   Installing to: $installDir" -ForegroundColor Gray
        Write-Host "   (For system-wide install, run as Administrator)" -ForegroundColor Gray
        Write-Host ""
    }
} else {
    $installDir = Join-Path $env:PROGRAMFILES $APP_NAME
    $needsAdmin = $true
}

# ── Fetch release info ────────────────────────────────────────────────────────
Write-Step "1." "Fetching latest release..."
$release = Get-DownloadUrl
Write-OK "Release: $($release.TagName) — $($release.Name)"

# ── Download ──────────────────────────────────────────────────────────────────
Write-Host ""
Write-Step "2." "Downloading..."
$tmpDir  = Join-Path $env:TEMP "TapeXPlayer-install"
New-Item -ItemType Directory -Force -Path $tmpDir | Out-Null
$zipPath = Join-Path $tmpDir $release.Name

Write-Host "   URL: $($release.Url)" -ForegroundColor Gray
try {
    # Invoke-WebRequest -UseBasicParsing is the most portable option:
    # - works in piped (iex) sessions where BITS jobs are unavailable
    # - follows redirects automatically (GitHub assets redirect to CDN)
    # - does not require Internet Explorer COM object (-UseBasicParsing)
    # - TLS 1.2 is guaranteed by the [Net.ServicePointManager] line above
    Invoke-WebRequest -Uri $release.Url -OutFile $zipPath -UseBasicParsing
} catch {
    Write-Fail "Download failed: $_"
}
Write-OK "Downloaded: $('{0:N1} MB' -f ((Get-Item $zipPath).Length / 1MB))"

# Optional SHA256
$shaUrl = $release.Url -replace '\.zip$', '.sha256'
Verify-SHA256 $zipPath $shaUrl

# ── Extract ───────────────────────────────────────────────────────────────────
Write-Host ""
Write-Step "3." "Extracting..."
$extractDir = Join-Path $tmpDir "extracted"
Remove-Item -Recurse -Force $extractDir -ErrorAction SilentlyContinue
Expand-Archive -Path $zipPath -DestinationPath $extractDir -Force

# Find the exe (may be in a subdirectory)
$exeFile = Get-ChildItem -Recurse -Path $extractDir -Filter "$APP_NAME.exe" | Select-Object -First 1
if (-not $exeFile) {
    Write-Fail "$APP_NAME.exe not found in the downloaded archive."
}
$sourceDir = $exeFile.DirectoryName
Write-OK "Extracted to: $sourceDir"

# ── Validate DLLs ─────────────────────────────────────────────────────────────
Write-Host ""
Write-Step "4." "Validating bundle..."
Validate-Bundle $sourceDir

# ── Install ───────────────────────────────────────────────────────────────────
Write-Host ""
Write-Step "5." "Installing to: $installDir"

# Handle existing installation
if (Test-Path $installDir) {
    if (-not $Force) {
        if (-not (Ask-YesNo "TapeXPlayer is already installed. Update?")) {
            Write-Host "   Installation cancelled." -ForegroundColor Yellow
            exit 0
        }
    }
    # Stop any running instance so the exe is not locked during overwrite
    $running = Get-Process -Name $APP_NAME -ErrorAction SilentlyContinue
    if ($running) {
        Write-Host "   Stopping running instance..." -ForegroundColor Gray
        $running | Stop-Process -Force
        Start-Sleep -Milliseconds 800
    }
    Write-Host "   Updating existing installation..." -ForegroundColor Gray
} else {
    New-Item -ItemType Directory -Force -Path $installDir | Out-Null
}

# Overwrite files in place — no full directory removal, preserves user data
Copy-Item -Path "$sourceDir\*" -Destination $installDir -Recurse -Force
Write-OK "Files copied to $installDir"

# ── Shortcuts ─────────────────────────────────────────────────────────────────
Write-Host ""
Write-Step "6." "Creating shortcuts..."

$exeTarget   = Join-Path $installDir "$APP_NAME.exe"
$desktop     = [Environment]::GetFolderPath("Desktop")
$startMenu   = Join-Path $env:APPDATA "Microsoft\Windows\Start Menu\Programs\$APP_NAME"

New-Item -ItemType Directory -Force -Path $startMenu | Out-Null
New-Shortcut (Join-Path $desktop      "$APP_NAME.lnk") $exeTarget "Professional Video Player"
New-Shortcut (Join-Path $startMenu    "$APP_NAME.lnk") $exeTarget "Professional Video Player"
Write-OK "Desktop shortcut created"
Write-OK "Start Menu entry created"

# ── Registry ──────────────────────────────────────────────────────────────────
$version = $release.TagName -replace '^v', ''
Register-UninstallEntry $installDir $version
Write-OK "Registered in Apps & Features"

# ── Cleanup ───────────────────────────────────────────────────────────────────
Remove-Item -Recurse -Force $tmpDir -ErrorAction SilentlyContinue

# ── Done ──────────────────────────────────────────────────────────────────────
Write-Host ""
Write-Host "================================================================" -ForegroundColor Cyan
Write-Host "   Installation complete!" -ForegroundColor Green
Write-Host "================================================================" -ForegroundColor Cyan
Write-Host ""
Write-Host "   $APP_NAME installed to: $installDir" -ForegroundColor White
Write-Host ""
Write-Host "   User data & settings:  $env:APPDATA\$APP_NAME" -ForegroundColor Gray
Write-Host "   Proxy cache (large):   $env:LOCALAPPDATA\$APP_NAME\cache" -ForegroundColor Gray
Write-Host "   (To free disk space — delete the cache folder above)" -ForegroundColor DarkGray
Write-Host ""
Write-Host "   Launch from:"
Write-Host "     - Desktop shortcut"
Write-Host "     - Start Menu -> $APP_NAME"
Write-Host "     - Run: $exeTarget"
Write-Host ""

if (-not $Quiet) {
    if (Ask-YesNo "Launch $APP_NAME now?") {
        Start-Process $exeTarget
    }
    Write-Host ""
    Read-Host "Press Enter to exit"
}
