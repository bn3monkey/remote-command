# install.ps1 - download a prebuilt remote-command server binary and install it.
#
# You MUST choose where the binary goes, so you always know what to delete later:
#   -Path <dir>     install into a directory you control (created if missing)
#   -DefaultPath    install into the built-in default (%LOCALAPPDATA%\Programs\remote-command)
#
# Usage (download then run, so the flags bind):
#   irm https://raw.githubusercontent.com/bn3monkey/remote-command/main/install.ps1 -OutFile install.ps1
#   .\install.ps1 -DefaultPath
#   .\install.ps1 -Path C:\tools\remote-command
#
# One-liner alternative:
#   & ([scriptblock]::Create((irm https://raw.githubusercontent.com/bn3monkey/remote-command/main/install.ps1))) -DefaultPath
#
# Supported platform: Windows, x64 (AMD64) only.
#Requires -Version 5
[CmdletBinding(DefaultParameterSetName = 'None')]
param(
    [Parameter(ParameterSetName = 'Custom', Mandatory = $true)]
    [string]$Path,

    [Parameter(ParameterSetName = 'Default', Mandatory = $true)]
    [switch]$DefaultPath,

    [string]$Repo = $(if ($env:REMOTE_COMMAND_REPO) { $env:REMOTE_COMMAND_REPO } else { 'bn3monkey/remote-command' }),
    [string]$Version = $(if ($env:REMOTE_COMMAND_VERSION) { $env:REMOTE_COMMAND_VERSION } else { 'latest' })
)
$ErrorActionPreference = 'Stop'

$DefaultInstallDir = Join-Path $env:LOCALAPPDATA 'Programs\remote-command'
$BinName = 'remote-command-server.exe'

# --- resolve install directory (a choice is mandatory) ---------------------
if ($PSCmdlet.ParameterSetName -eq 'None') {
    Write-Host "error: choose an install location: pass -Path <dir> or -DefaultPath" -ForegroundColor Red
    Write-Host "       (default would be: $DefaultInstallDir)"
    exit 1
}
$InstallDir = if ($DefaultPath) { $DefaultInstallDir } else { $Path }

# --- detect platform -------------------------------------------------------
if ($env:PROCESSOR_ARCHITECTURE -ne 'AMD64') {
    throw "Unsupported architecture '$env:PROCESSOR_ARCHITECTURE' (only x64/AMD64 is supported)."
}

$asset = 'remote_command_server-windows-x86_64.exe'
if ($Version -eq 'latest') {
    $base = "https://github.com/$Repo/releases/latest/download"
} else {
    $base = "https://github.com/$Repo/releases/download/$Version"
}

# --- download --------------------------------------------------------------
# The download dir lives INSIDE the install path (".dl"), not in %TEMP%, so the
# behavior matches install.sh and does not depend on the environment's temp dir.
New-Item -ItemType Directory -Force -Path $InstallDir | Out-Null
$tmp = Join-Path $InstallDir '.dl'
if (Test-Path $tmp) { Remove-Item -Recurse -Force $tmp }
New-Item -ItemType Directory -Force -Path $tmp | Out-Null
try {
    Write-Host "Downloading $asset ($Version) from $Repo ..."
    Invoke-WebRequest "$base/$asset"        -OutFile (Join-Path $tmp $asset)
    Invoke-WebRequest "$base/$asset.sha256" -OutFile (Join-Path $tmp "$asset.sha256")

    # --- verify checksum ---------------------------------------------------
    Write-Host "Verifying checksum ..."
    $expected = (((Get-Content (Join-Path $tmp "$asset.sha256") -Raw).Trim()) -split '\s+')[0].ToLower()
    $actual = (Get-FileHash (Join-Path $tmp $asset) -Algorithm SHA256).Hash.ToLower()
    if ($expected -ne $actual) {
        throw "Checksum mismatch! expected=$expected actual=$actual"
    }

    # --- install -----------------------------------------------------------
    $dest = Join-Path $InstallDir $BinName
    Copy-Item (Join-Path $tmp $asset) $dest -Force
    Write-Host "Installed: $dest"
    Write-Host "To uninstall:  Remove-Item `"$dest`""

    $onPath = ($env:Path -split ';') | Where-Object { $_ -eq $InstallDir }
    if (-not $onPath) {
        Write-Host "NOTE: '$InstallDir' is not on your PATH. Add it, e.g.:"
        Write-Host "      setx PATH `"$InstallDir;`$env:PATH`""
    }

    Write-Host "Run with:  $dest [discovery_port] [command_port] [stream_port] [working_dir]"
}
finally {
    Remove-Item -Recurse -Force $tmp
}
