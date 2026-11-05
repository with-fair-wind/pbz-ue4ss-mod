#Requires -Version 7.0
[CmdletBinding()]
param([Parameter(Mandatory)][string]$EvidenceDirectory)
$ErrorActionPreference = 'Stop'
$root = Split-Path (Split-Path $PSScriptRoot -Parent) -Parent
. (Join-Path $root 'scripts/runtime.ps1')
$fixture = Join-Path $root ('.qa-' + [Guid]::NewGuid().ToString('N').Substring(0, 8))
$game = Join-Path $fixture 'game space'
$destination = Join-Path $game 'platform/steam'
$backupRoot = Join-Path $root 'build/deploy-backups'
$ownedBackups = @{}
$originalSave = ${function:Save-PBZDeliveryReceipt}
$originalUnlock = ${function:Assert-PBZUnlocked}
function Save-PBZDeliveryReceipt([hashtable]$Receipt, [string]$Path) {
    if ($Receipt.status -eq 'prepared' -and $Receipt.installDirectory -ieq $game) {
        $dir = Split-Path $Path -Parent
        if ((Split-Path $dir -Parent) -ine $backupRoot -or (Split-Path $dir -Leaf) -cnotmatch '^[0-9a-f]{32}$') {
            throw 'Invalid fixture backup directory.'
        }
        # Cleanup uses only filenames bounded by the operation that created the backup.
        $count = if ($Receipt.mode -eq 'install') { $runtimeFiles.Count } else { 2 }
        $ownedBackups[$dir] = @(for ($i = 0; $i -lt $count; $i++) {
            if ($Receipt.files[$i].existed) { "$i.bin" }
        })
    }
    & $originalSave $Receipt $Path
}
$cases = [Collections.Generic.List[object]]::new()
$ownedFiles = [Collections.Generic.HashSet[string]]::new([StringComparer]::OrdinalIgnoreCase)
$originalCopy = ${function:Copy-PBZFile}
$originalVerify = ${function:Assert-PBZFile}
$version = (Get-PBZProfile $root).manifest.gameFileVersion
function Get-PBZGameVersion([string]$Executable) {
    if (-not (Test-Path -LiteralPath $Executable -PathType Leaf)) { throw 'Fixture executable missing.' }
    return $script:version
}
function Assert-FixturePath([string]$Base, [string]$Path) {
    $full = [IO.Path]::GetFullPath($Path)
    if (-not $full.StartsWith([IO.Path]::GetFullPath($Base) + '\', [StringComparison]::OrdinalIgnoreCase)) {
        throw "Cleanup bounds failed: $full"
    }
    Assert-PBZPaths @($full)
}
function Remove-FixtureFile([string]$Base, [string]$Path) {
    Assert-FixturePath $Base $Path
    if (Test-Path -LiteralPath $Path) { [IO.File]::Delete($Path); $deleted.Add($Path) }
}
function Remove-FixtureBackup([string]$Directory) {
    if (-not $ownedBackups.ContainsKey($Directory)) { throw 'Unowned backup directory.' }
    Assert-FixturePath $backupRoot $Directory
    $files = @('receipt.json') + $ownedBackups[$Directory]
    foreach ($name in $files) { Assert-FixturePath $Directory (Join-Path $Directory $name) }
    foreach ($name in $files) { Remove-FixtureFile $Directory (Join-Path $Directory $name) }
    if ((Test-Path -LiteralPath $Directory) -and @(Get-ChildItem -LiteralPath $Directory -Force).Count -eq 0) {
        [IO.Directory]::Delete($Directory, $false)
        $removedDirectories.Add($Directory)
    }
}
function Put([string]$Relative, [string]$Text) {
    $path = Join-Path $destination $Relative
    Assert-FixturePath $fixture $path
    [IO.Directory]::CreateDirectory((Split-Path $path -Parent)) | Out-Null
    [IO.File]::WriteAllText($path, $Text)
    $null = $ownedFiles.Add($path)
}
function Check([bool]$Condition, [string]$Message) { if (-not $Condition) { throw $Message } }
function Expect-Failure([scriptblock]$Action, [string]$Pattern) {
    $message = ''
    try { & $Action | Out-Null } catch { $message = $_.Exception.Message }
    Check ($message -match $Pattern) "Expected '$Pattern', received '$message'"
    return $message
}
function Case([string]$Name, [scriptblock]$Action) {
    & $Action
    $cases.Add(@{scenario=$Name; observable='PASS'; invocation='deploy_tests.ps1: ' + $Name})
    Write-Host "PASS $Name"
}
function Deliver { Invoke-PBZDelivery -Root $root -InstallDirectory $game }
function Pair-Old { Put 'ue4ss/UE4SS.dll' 'old-runtime'; Put 'ue4ss/Mods/PBZEditor/dlls/main.dll' 'old-mod' }
function Assert-Old {
    Check (([IO.File]::ReadAllText((Join-Path $destination 'ue4ss/UE4SS.dll'))) -ceq 'old-runtime') 'Runtime was not restored'
    Check (([IO.File]::ReadAllText((Join-Path $destination 'ue4ss/Mods/PBZEditor/dlls/main.dll'))) -ceq 'old-mod') 'Mod was not restored'
}
$sentinel = Join-Path $fixture 'outside-game-sentinel.txt'
$sourceSentinel = Join-Path $root 'mods/PBZEditor/src/dllmain.cpp'
$sourceHash = Get-PBZHash $sourceSentinel
Assert-FixturePath $root $fixture
Assert-FixturePath $fixture $sentinel
if (Test-Path -LiteralPath $fixture) { throw 'Fixture collision' }
[IO.Directory]::CreateDirectory((Split-Path $sentinel -Parent)) | Out-Null
[IO.File]::WriteAllText($sentinel, 'outside fixture preservation')
$sentinelHash = Get-PBZHash $sentinel
$null = $ownedFiles.Add($sentinel)
$deleted = [Collections.Generic.List[string]]::new()
$removedDirectories = [Collections.Generic.List[string]]::new()
$cleanup = @{}
try {
    Assert-PBZPaths @($fixture, $sentinel)
    Put 'PBZ.exe' 'test executable: version boundary replaced only inside test scope'
    Put 'ue4ss/Mods/Other/sentinel.txt' 'keep other mod'
    $otherHash = Get-PBZHash (Join-Path $destination 'ue4ss/Mods/Other/sentinel.txt')
    $runtime = New-PBZRuntime $root ''
    $runtimeFiles = @((Get-PBZRuntimeFiles $root (Join-Path $root 'build/msvc')).Keys) + @('ue4ss/Mods/mods.txt', 'ue4ss/Mods/PBZEditor/enabled.txt')
    foreach ($relative in $runtimeFiles) { $null = $ownedFiles.Add((Join-Path $destination $relative)) }
    Case 'source staging hashes and exactly two enabled mods' {
        $map = Get-PBZRuntimeFiles $root (Join-Path $root 'build/msvc')
        foreach ($relative in $map.Keys) { Check ((Get-PBZHash (Join-Path $runtime $relative)) -ceq (Get-PBZHash $map[$relative])) "Staging hash $relative" }
        Check (([IO.File]::ReadAllText((Join-Path $runtime 'ue4ss/Mods/mods.txt'))) -ceq "Keybinds : 1`n") 'Unexpected mods.txt'
        Check ((Get-Item (Join-Path $runtime 'ue4ss/Mods/PBZEditor/enabled.txt')).Length -eq 0) 'Marker is not empty'
        Check (@(Get-ChildItem (Join-Path $runtime 'ue4ss/Mods') -Directory).Count -eq 2) 'Unexpected mod directory'
    }
    Case 'initial install then explicit restore restores absence' {
        $receipt = Invoke-PBZDelivery -Root $root -InstallDirectory $game -InitialInstall
        Check (Test-Path (Join-Path $destination 'ue4ss/Mods/PBZEditor/enabled.txt')) 'Initial install marker missing'
        Invoke-PBZDelivery -Root $root -Restore $receipt | Out-Null
        foreach ($relative in $runtimeFiles) { Check (-not (Test-Path (Join-Path $destination $relative))) "Expected absent: $relative" }
    }
    Case 'initial existing proxy and new file failure restores original presence' {
        Put 'dwmapi.dll' 'old-proxy'
        $script:hit = 0
        function Copy-PBZFile([string]$Source, [string]$Destination) {
            if ($Source -like '*build\runtime\*' -and $Destination -like '*PBZEditor*dlls*main.dll') { $script:hit++; throw 'injected initial file failure' }
            & $script:originalCopy $Source $Destination
        }
        Expect-Failure { Invoke-PBZDelivery -Root $root -InstallDirectory $game -InitialInstall } 'original files restored' | Out-Null
        Check ($script:hit -eq 1) 'Initial fault not reached'
        Check (([IO.File]::ReadAllText((Join-Path $destination 'dwmapi.dll'))) -ceq 'old-proxy') 'Proxy not restored'
        Check (-not (Test-Path (Join-Path $destination 'ue4ss/UE4SS.dll'))) 'New runtime not removed'
    }
    Case 'initial install replaces old adaptation and explicit restore returns its bytes' {
        Put 'ue4ss/MemberVariableLayout.ini' 'old-layout'
        $receipt = Invoke-PBZDelivery -Root $root -InstallDirectory $game -InitialInstall
        Check ((Get-PBZHash (Join-Path $destination 'ue4ss/MemberVariableLayout.ini')) -ceq (Get-PBZHash (Join-Path $runtime 'ue4ss/MemberVariableLayout.ini'))) 'Initial install did not repair profile'
        Invoke-PBZDelivery -Root $root -Restore $receipt | Out-Null
        Check (([IO.File]::ReadAllText((Join-Path $destination 'ue4ss/MemberVariableLayout.ini'))) -ceq 'old-layout') 'Old profile not restored'
    }
    Invoke-PBZDelivery -Root $root -InstallDirectory $game -InitialInstall | Out-Null
    Pair-Old
    $stable = @{}
    foreach ($relative in @('dwmapi.dll', 'ue4ss/UE4SS-settings.ini', 'ue4ss/MemberVariableLayout.ini', 'ue4ss/Mods/mods.txt')) { $stable[$relative] = Get-PBZHash (Join-Path $destination $relative) }
    Case 'second DLL exclusive lock leaves first untouched' {
        $handle = [IO.File]::Open((Join-Path $destination 'ue4ss/Mods/PBZEditor/dlls/main.dll'), 'Open', 'ReadWrite', 'None')
        try { Expect-Failure { Deliver } 'used by another process' | Out-Null } finally { $handle.Dispose() }
        Assert-Old
    }
    Case 'daily success repeat and explicit restore' {
        $receipt = Deliver
        $second = Deliver
        Check ($receipt -ne $second) 'Repeated deployment did not produce a distinct backup'
        Check ((Get-PBZHash (Join-Path $destination 'ue4ss/UE4SS.dll')) -ceq (Get-PBZHash (Join-Path $runtime 'ue4ss/UE4SS.dll'))) 'Runtime copy mismatch'
        Invoke-PBZDelivery -Root $root -Restore $receipt | Out-Null
        Assert-Old
        foreach ($relative in $stable.Keys) { Check ((Get-PBZHash (Join-Path $destination $relative)) -ceq $stable[$relative]) "Daily changed $relative" }
    }
    Case 'daily missing marker refuses before writes' {
        Remove-FixtureFile $fixture (Join-Path $destination 'ue4ss/Mods/PBZEditor/enabled.txt')
        Expect-Failure { Deliver } 'Install the full runtime first' | Out-Null
        Assert-Old
        Put 'ue4ss/Mods/PBZEditor/enabled.txt' ''
    }
    Case 'daily missing mod DLL refuses before writes' {
        Remove-FixtureFile $fixture (Join-Path $destination 'ue4ss/Mods/PBZEditor/dlls/main.dll')
        Expect-Failure { Deliver } 'Install the full runtime first' | Out-Null
        Check (([IO.File]::ReadAllText((Join-Path $destination 'ue4ss/UE4SS.dll'))) -ceq 'old-runtime') 'Missing mod changed runtime'
        Put 'ue4ss/Mods/PBZEditor/dlls/main.dll' 'old-mod'
    }
    Case 'second copy failure restores old pair' {
        $script:hit = 0
        function Copy-PBZFile([string]$Source, [string]$Destination) {
            if ($Source -like '*build\runtime\*' -and $Destination -like '*PBZEditor*dlls*main.dll') { $script:hit++; throw 'injected second swap' }
            & $script:originalCopy $Source $Destination
        }
        Expect-Failure { Deliver } 'original files restored' | Out-Null
        Check ($script:hit -eq 1) 'Second swap fault not reached'
        Assert-Old
    }
    Case 'post-copy verification failure restores old pair' {
        $script:hit = 0
        function Assert-PBZFile([string]$Path, [string]$Hash) {
            if ($Path -like '*game space*PBZEditor*dlls*main.dll' -and $Hash -ne (Get-PBZHash (Join-Path $destination 'ue4ss/UE4SS.dll')) -and $script:hit -eq 0) { $script:hit++; throw 'injected verify failure' }
            & $script:originalVerify $Path $Hash
        }
        Expect-Failure { Deliver } 'original files restored' | Out-Null
        Check ($script:hit -eq 1) 'Verification fault not reached'
        Assert-Old
    }
    Case 'failed restore continues remaining files and records actual state' {
        $script:hit = 0; $script:remaining = 0
        function Copy-PBZFile([string]$Source, [string]$Destination) {
            if ($Source -like '*build\runtime\*' -and $Destination -like '*PBZEditor*dlls*main.dll') { throw 'injected delivery failure' }
            if ($Source -like '*deploy-backups*0.bin') { $script:hit++; throw 'injected restore failure' }
            if ($Source -like '*deploy-backups*1.bin') { $script:remaining++ }
            & $script:originalCopy $Source $Destination
        }
        Expect-Failure { Deliver } 'Recovery incomplete' | Out-Null
        Check ($script:hit -eq 1 -and $script:remaining -eq 1) 'Restore stopped before remaining item'
        $latest = $ownedBackups.Keys | ForEach-Object { Get-Item -LiteralPath (Join-Path $_ 'receipt.json') } |
            Sort-Object LastWriteTimeUtc -Descending | Select-Object -First 1
        $r = Get-Content $latest.FullName -Raw | ConvertFrom-Json
        Check ($r.status -eq 'restore-failed' -and $r.restoreErrors.Count -eq 1) 'Recovery status missing'
        Check ($r.files[0].actualHash -eq (Get-PBZHash (Join-Path $destination 'ue4ss/UE4SS.dll'))) 'Actual state not recorded'
    }
    Pair-Old
    Case 'unsupported game version refuses before writes' {
        $script:version = 'unsupported'
        try { Expect-Failure { Deliver } 'Unsupported PBZ' | Out-Null; Assert-Old } finally { $script:version = (Get-PBZProfile $root).manifest.gameFileVersion }
    }
    Case 'running game refuses before writes' {
        function Assert-PBZStopped { throw 'Fully exit PBZ test process boundary' }
        Expect-Failure { Deliver } 'Fully exit PBZ' | Out-Null
        Assert-Old
    }
    Case 'installed profile mismatch refuses before writes' {
        Put 'ue4ss/MemberVariableLayout.ini' 'wrong'
        try { Expect-Failure { Deliver } 'Readback mismatch' | Out-Null; Assert-Old }
        finally { [IO.File]::Copy((Join-Path $runtime 'ue4ss/MemberVariableLayout.ini'), (Join-Path $destination 'ue4ss/MemberVariableLayout.ini'), $true) }
    }
    foreach ($mutation in @(
        @{name='build receipt'; relative='build/msvc/build-info.json'; pattern='Invalid or stale'; transform={param($s) $s.Replace('PBZ-source-build', 'wrong-purpose')}},
        @{name='source hash'; relative='mods/PBZEditor/src/dllmain.cpp'; pattern='Source changed'; transform={param($s) $s + "`n"}},
        @{name='profile hash'; relative='game-configs/PhantomBladeZero/MemberVariableLayout.ini'; pattern='Profile hash mismatch'; transform={param($s) $s + "`n"}}
    )) {
        Case "$($mutation.name) refusal before writes" {
            $path = Join-Path $root $mutation.relative
            $bytes = [IO.File]::ReadAllBytes($path)
            try {
                [IO.File]::WriteAllText($path, (& $mutation.transform ([IO.File]::ReadAllText($path))))
                Expect-Failure { Deliver } $mutation.pattern | Out-Null
                Assert-Old
            } finally { [IO.File]::WriteAllBytes($path, $bytes) }
        }
    }
    Case 'build DLL hash refusal before writes' {
        $path = Join-Path $root 'build/msvc/Game__Shipping__Win64/bin/PBZEditor.dll'
        $bytes = [IO.File]::ReadAllBytes($path)
        try { [IO.File]::WriteAllText($path, 'changed'); Expect-Failure { Deliver } 'Build artifact changed' | Out-Null; Assert-Old }
        finally { [IO.File]::WriteAllBytes($path, $bytes) }
    }
    Case 'restore rejects outside or corrupted receipt before writes' {
        Expect-Failure { Invoke-PBZDelivery -Root $root -Restore (Join-Path $fixture 'receipt.json') } 'Restore requires' | Out-Null
        $receipt = Deliver
        $bytes = [IO.File]::ReadAllBytes($receipt)
        try {
            [IO.File]::WriteAllText($receipt, ([IO.File]::ReadAllText($receipt).Replace('PBZ-delivery', 'wrong-purpose')))
            Expect-Failure { Invoke-PBZDelivery -Root $root -Restore $receipt } 'Invalid delivery receipt' | Out-Null
        } finally { [IO.File]::WriteAllBytes($receipt, $bytes) }
        Invoke-PBZDelivery -Root $root -Restore $receipt | Out-Null
        Assert-Old
    }
    Case 'corrupted backup refuses restore before writes' {
        $receipt = Deliver
        $backup = Join-Path (Split-Path $receipt -Parent) '0.bin'
        $bytes = [IO.File]::ReadAllBytes($backup)
        $before = Get-PBZHash (Join-Path $destination 'ue4ss/UE4SS.dll')
        try {
            [IO.File]::WriteAllText($backup, 'corrupt backup')
            Expect-Failure { Invoke-PBZDelivery -Root $root -Restore $receipt } 'Readback mismatch' | Out-Null
            Check ((Get-PBZHash (Join-Path $destination 'ue4ss/UE4SS.dll')) -ceq $before) 'Corrupted backup modified target'
        } finally { [IO.File]::WriteAllBytes($backup, $bytes) }
        Invoke-PBZDelivery -Root $root -Restore $receipt | Out-Null
        Assert-Old
    }
    Case 'cross-volume and repository collision refuse before writes' {
        $otherDrive = if ([IO.Path]::GetPathRoot($root) -ine 'C:\') { 'C:\PBZ-unwritten-fixture' } else { 'D:\PBZ-unwritten-fixture' }
        Expect-Failure { Invoke-PBZDelivery -Root $root -InstallDirectory $otherDrive } 'same volume' | Out-Null
        Expect-Failure { Invoke-PBZDelivery -Root $root -InstallDirectory (Join-Path $root 'build') } 'collides' | Out-Null
        Assert-Old
    }
    Case 'reparse game ancestor refuses before writes' {
        $link = Join-Path $fixture 'linked game'
        Assert-FixturePath $fixture $link
        New-Item -ItemType Junction -Path $link -Target $game | Out-Null
        try { Expect-Failure { Invoke-PBZDelivery -Root $root -InstallDirectory $link } 'Reparse paths' | Out-Null; Assert-Old }
        finally {
            Assert-FixturePath $root (Split-Path $link -Parent)
            $junction = Get-Item -LiteralPath $link -Force
            Check ($junction.LinkType -eq 'Junction' -and $junction.Target -ieq $game) 'Unexpected fixture link'
            [IO.Directory]::Delete($link, $false)
            $removedDirectories.Add($link)
        }
    }
    Case 'unexpected staged file is preserved and refused' {
        $path = Join-Path $runtime 'unexpected.txt'
        Assert-FixturePath $runtime $path
        if (Test-Path -LiteralPath $path) { throw 'Staging sentinel collision' }
        [IO.File]::WriteAllText($path, 'keep')
        try {
            Expect-Failure { New-PBZRuntime $root '' } 'Unexpected staging file preserved' | Out-Null
            Check (([IO.File]::ReadAllText($path)) -ceq 'keep') 'Unknown staging file changed'
        } finally { Remove-FixtureFile $runtime $path }
    }
    Case 'CLI real version check and missing receipt exit nonzero' {
        foreach ($script in @('deploy.ps1', 'install-runtime.ps1')) {
            & pwsh -NoProfile -File (Join-Path $root "scripts/$script") -InstallDirectory $game *> (Join-Path $EvidenceDirectory "task-5-cli-$script.log")
            Check ($LASTEXITCODE -ne 0) 'CLI accepted fake executable'
            Check ((Get-Content (Join-Path $EvidenceDirectory "task-5-cli-$script.log") -Raw) -match 'Unsupported PBZ') 'CLI did not reach real version check'
        }
        & pwsh -NoProfile -File (Join-Path $root 'scripts/deploy.ps1') -Restore (Join-Path $fixture 'receipt.json') *> (Join-Path $EvidenceDirectory 'task-5-cli-restore.log')
        Check ($LASTEXITCODE -ne 0) 'CLI accepted outside receipt'
    }
    foreach ($boundary in @('process', 'lock')) {
        Case "final $boundary refusal leaves both targets untouched" {
            $script:checks = 0
            $script:writes = 0
            function Copy-PBZFile($Source, $Destination) {
                $script:writes++
                & $originalCopy $Source $Destination
            }
            if ($boundary -eq 'process') {
                function Assert-PBZStopped {
                    if (++$script:checks -eq 2) { throw 'fixture final process refusal' }
                }
            } else {
                function Assert-PBZUnlocked($Paths) {
                    if (++$script:checks -eq 2) {
                        $handle = [IO.File]::Open($Paths[1], 'Open', 'ReadWrite', 'None')
                        try { & $originalUnlock $Paths } finally { $handle.Dispose() }
                    } else { & $originalUnlock $Paths }
                }
            }
            Expect-Failure { Deliver } 'final process refusal|used by another process' | Out-Null
            Check ($script:checks -eq 2 -and $script:writes -eq 0) 'Final refusal wrote a target'
            Assert-Old
        }
    }
    Case 'partial first copy restores only attempted target' {
        $script:restored = [Collections.Generic.List[string]]::new()
        $script:hit = 0
        function Copy-PBZFile($Source, $Destination) {
            if ($Source -like '*build\runtime\*') {
                [IO.File]::WriteAllText($Destination, 'partial')
                $script:hit++
                throw 'injected partial first swap'
            }
            $script:restored.Add($Source)
            & $originalCopy $Source $Destination
        }
        Expect-Failure { Deliver } 'original files restored' | Out-Null
        Check ($script:hit -eq 1 -and $script:restored.Count -eq 1 -and $script:restored[0] -like '*0.bin') 'Wrong rollback subset'
        Assert-Old
    }
    foreach ($index in @(0, 1)) {
        Case "malformed daily entry $index refuses entire pair" {
            $receipt = Deliver
            $pristine = Get-Content $receipt -Raw
            $r = $pristine | ConvertFrom-Json -AsHashtable
            $expected = @($r.files | ForEach-Object { Get-PBZHash (Join-Path $destination $_.path) })
            $script:writes = 0
            function Copy-PBZFile($Source, $Destination) { $script:writes++; & $originalCopy $Source $Destination }
            try {
                foreach ($invalid in @('flag', 'absent', 'name', 'hash')) {
                    $r = $pristine | ConvertFrom-Json -AsHashtable
                    $item = $r.files[$index]
                    switch ($invalid) {
                        'flag' { $item.existed = $false }
                        'absent' { $item.existed = $false; $item.originalHash = $null }
                        'name' { $item.backup = '../sentinel' }
                        'hash' { $item.originalHash = '0' * 64 }
                    }
                    Save-PBZDeliveryReceipt $r $receipt
                    Expect-Failure { Invoke-PBZDelivery -Root $root -Restore $receipt } 'Invalid backup entry|Readback mismatch' | Out-Null
                    for ($i = 0; $i -lt 2; $i++) {
                        Check ((Get-PBZHash (Join-Path $destination $r.files[$i].path)) -ceq $expected[$i]) 'Malformed receipt changed pair'
                    }
                    Check ($script:writes -eq 0) 'Malformed restore reached target writer'
                }
            } finally { [IO.File]::WriteAllText($receipt, $pristine) }
            Invoke-PBZDelivery -Root $root -Restore $receipt | Out-Null
            Assert-Old
        }
    }
    Case 'install absence conflicts with a numbered backup before any restore' {
        $receipt = Invoke-PBZDelivery -Root $root -InstallDirectory $game -InitialInstall
        $pristine = Get-Content $receipt -Raw
        $r = $pristine | ConvertFrom-Json -AsHashtable
        $item = $r.files[-1]
        Check $item.existed 'Expected numbered backup for present original'
        $item.existed = $false
        $item.originalHash = $null
        Save-PBZDeliveryReceipt $r $receipt
        try {
            Expect-Failure { Invoke-PBZDelivery -Root $root -Restore $receipt } 'Unexpected backup for absent original' | Out-Null
            foreach ($file in $r.files) { Assert-PBZFile (Join-Path $destination $file.path) $file.installedHash }
        } finally { [IO.File]::WriteAllText($receipt, $pristine) }
        Invoke-PBZDelivery -Root $root -Restore $receipt | Out-Null
        Assert-Old
    }
    Case 'malicious cleanup receipt cannot escape or delete an unknown entry' {
        $receipt = Deliver
        $dir = Split-Path $receipt -Parent
        Invoke-PBZDelivery -Root $root -Restore $receipt | Out-Null
        $r = Get-Content $receipt -Raw | ConvertFrom-Json -AsHashtable
        $r.files[0].backup = [IO.Path]::GetRelativePath($dir, $sentinel)
        Save-PBZDeliveryReceipt $r $receipt
        $unknown = Join-Path $dir 'unknown.txt'
        Assert-FixturePath $dir $unknown
        [IO.File]::WriteAllText($unknown, 'unknown')
        Remove-FixtureBackup $dir
        Check ((Get-PBZHash $sentinel) -ceq $sentinelHash -and [IO.File]::ReadAllText($unknown) -ceq 'unknown') 'Cleanup removed foreign content'
        Check (-not (Test-Path $receipt) -and -not (Test-Path (Join-Path $dir '0.bin'))) 'Owned files survived cleanup'
        Remove-FixtureFile $dir $unknown
    }
    Check ($cases.Count -eq 31) "Unexpected scenario count $($cases.Count)"
    Check ((Get-PBZHash (Join-Path $destination 'ue4ss/Mods/Other/sentinel.txt')) -ceq $otherHash) 'Other mod changed'
    Check ((Get-PBZHash $sentinel) -ceq $sentinelHash -and (Get-PBZHash $sourceSentinel) -ceq $sourceHash) 'Outside sentinel changed'
    @{status='PASS'; scenarios=$cases.ToArray(); invocation='pwsh -NoProfile -File tests/scripts/deploy_tests.ps1 -EvidenceDirectory <E>'; game='not touched'} | ConvertTo-Json -Depth 8 | Set-Content (Join-Path $EvidenceDirectory 'task-5-deploy-tests.json') -Encoding utf8NoBOM
} finally {
    $sentinelUnchanged = (Get-PBZHash $sentinel) -ceq $sentinelHash
    $directories = [Collections.Generic.HashSet[string]]::new([StringComparer]::OrdinalIgnoreCase)
    foreach ($path in $ownedFiles) {
        Assert-FixturePath $fixture $path
        $parent = Split-Path $path -Parent
        while ($parent -ine $fixture) {
            Assert-FixturePath $fixture $parent
            $null = $directories.Add($parent)
            $parent = Split-Path $parent -Parent
        }
    }
    foreach ($path in $ownedFiles) { Remove-FixtureFile $fixture $path }
    foreach ($dir in $ownedBackups.Keys) { Remove-FixtureBackup $dir }
    $null = $directories.Add($fixture)
    foreach ($dir in @($directories | Sort-Object Length -Descending)) {
        Assert-FixturePath $root $dir
        if ((Test-Path -LiteralPath $dir) -and @(Get-ChildItem -LiteralPath $dir -Force).Count -eq 0) {
            [IO.Directory]::Delete($dir, $false)
            $removedDirectories.Add($dir)
        }
    }
    $cleanup = @{
        fixture=$fixture; fixtureRemoved=(-not (Test-Path $fixture)); removedFiles=$deleted.ToArray()
        removedDirectories=$removedDirectories.ToArray(); ownedBackupDirectories=@($ownedBackups.Keys)
        sourceSentinelUnchanged=((Get-PBZHash $sourceSentinel) -ceq $sourceHash); evidenceSentinelUnchanged=$sentinelUnchanged
    }
    $cleanup | ConvertTo-Json -Depth 5 | Set-Content (Join-Path $EvidenceDirectory 'task-5-cleanup.json') -Encoding utf8NoBOM
}
