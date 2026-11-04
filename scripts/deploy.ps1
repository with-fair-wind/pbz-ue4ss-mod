#Requires -Version 7.0
<#
.SYNOPSIS
Updates only the source-built UE4SS and PBZEditor DLL pair, or restores a backup.
.DESCRIPTION
InstallDirectory is the game root containing platform/steam/PBZ.exe. Exit PBZ first.
Backups remain in this repository's build/deploy-backups. Restore accepts its receipt.json.
File preflight and verified rollback do not guarantee atomicity across files or power loss.
#>
[CmdletBinding(DefaultParameterSetName='Deploy')]
param(
    [Parameter(Mandatory, ParameterSetName='Deploy')][string]$InstallDirectory,
    [Parameter(ParameterSetName='Deploy')][string]$BuildDirectory,
    [Parameter(Mandatory, ParameterSetName='Restore')][string]$Restore
)
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'runtime.ps1')
try {
    $receipt = Invoke-PBZDelivery -Root (Split-Path $PSScriptRoot -Parent) -InstallDirectory $InstallDirectory -BuildDirectory $BuildDirectory -Restore $Restore
    Write-Host "Completed. Recovery receipt: $receipt"
} catch { Write-Error $_ -ErrorAction Continue; exit 1 }
