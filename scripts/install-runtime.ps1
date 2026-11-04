#Requires -Version 7.0
<#
.SYNOPSIS
Explicitly installs the complete source-built PBZ runtime, or restores a backup.
.DESCRIPTION
InstallDirectory is the game root containing platform/steam/PBZ.exe. Exit PBZ first.
Installs proxy, runtime, settings, PBZ adaptation, Keybinds and PBZEditor. Unknown files
are preserved. Previous adaptation is backed up. Backups are in build/deploy-backups.
Use deploy.ps1 for subsequent DLL-only updates. Restore accepts a backup receipt.json.
File preflight and verified rollback do not guarantee atomicity across files or power loss.
#>
[CmdletBinding(DefaultParameterSetName='Install')]
param(
    [Parameter(Mandatory, ParameterSetName='Install')][string]$InstallDirectory,
    [Parameter(ParameterSetName='Install')][string]$BuildDirectory,
    [Parameter(Mandatory, ParameterSetName='Restore')][string]$Restore
)
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'runtime.ps1')
try {
    $receipt = Invoke-PBZDelivery -Root (Split-Path $PSScriptRoot -Parent) -InstallDirectory $InstallDirectory -BuildDirectory $BuildDirectory -InitialInstall -Restore $Restore
    Write-Host "Completed. Recovery receipt: $receipt"
} catch { Write-Error $_ -ErrorAction Continue; exit 1 }
