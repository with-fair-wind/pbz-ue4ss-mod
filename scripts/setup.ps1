#Requires -Version 7.0
[CmdletBinding()]
param()

$ErrorActionPreference = 'Stop'
$repositoryRoot = Split-Path -Parent $PSScriptRoot

function Invoke-Git {
    param([string]$Directory, [string[]]$Arguments)
    $output = & git.exe -C $Directory @Arguments
    if ($LASTEXITCODE -ne 0) {
        throw "Git failed (exit $LASTEXITCODE): git -C `"$Directory`" $($Arguments -join ' ')"
    }
    return $output
}

function Assert-CleanDependency {
    param([string]$Directory)
    if (-not (Test-Path -LiteralPath "$Directory/.git")) {
        if ((Test-Path -LiteralPath $Directory) -and
            @(Get-ChildItem -LiteralPath $Directory -Force).Count -gt 0) {
            throw "Dependency directory is not an initialized submodule and is not empty: $Directory"
        }
        return
    }

    $changes = @(Invoke-Git $Directory @('status', '--porcelain=v1', '--untracked-files=all', '--ignore-submodules=all'))
    if ($changes.Count -gt 0) {
        throw "Dirty dependency preserved; commit or move your changes before setup: $Directory`n$($changes -join "`n")"
    }
    foreach ($entry in @(Invoke-Git $Directory @('ls-files', '--stage'))) {
        if ($entry -match '^160000 [0-9a-f]{40} 0\t(.+)$') {
            Assert-CleanDependency (Join-Path $Directory $Matches[1])
        }
    }
}

try {
    $gitlink = @(Invoke-Git $repositoryRoot @('ls-files', '--stage', '--', 'RE-UE4SS'))
    if ($gitlink.Count -ne 1 -or $gitlink[0] -notmatch '^160000 ([0-9a-f]{40}) 0\tRE-UE4SS$') {
        throw 'The parent repository must record RE-UE4SS as one stage-0 Git submodule.'
    }
    $expectedCommit = $Matches[1]
    Assert-CleanDependency (Join-Path $repositoryRoot 'RE-UE4SS')

    # Upstream SSH URLs use HTTPS for this invocation only; pins come from gitlinks.
    Invoke-Git $repositoryRoot @(
        '-c', 'url.https://github.com/.insteadOf=git@github.com:',
        '-c', 'url.https://github.com/.insteadOf=ssh://git@github.com/',
        'submodule', 'update', '--init', '--recursive', '--', 'RE-UE4SS'
    ) | Write-Host

    $actualCommit = Invoke-Git (Join-Path $repositoryRoot 'RE-UE4SS') @('rev-parse', 'HEAD')
    if ($actualCommit -ne $expectedCommit) {
        throw "RE-UE4SS HEAD does not match parent gitlink: $expectedCommit"
    }
    $states = @(Invoke-Git $repositoryRoot @('submodule', 'status', '--recursive', '--', 'RE-UE4SS'))
    if ($states.Count -eq 0 -or @($states | Where-Object { $_ -notmatch '^ [0-9a-f]{40} ' }).Count -gt 0) {
        throw "Recursive submodules do not match their recorded gitlinks:`n$($states -join "`n")"
    }
    $states | Write-Host
    Write-Host "Dependencies ready at parent-recorded commit $expectedCommit."
}
catch {
    Write-Error $_ -ErrorAction Continue
    exit 1
}
