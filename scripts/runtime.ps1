# Shared source packaging and the two explicit PBZ delivery entry points.
. (Join-Path $PSScriptRoot 'profile.ps1')

function Get-PBZHash([string]$Path) {
    return (Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash.ToLowerInvariant()
}

function Assert-PBZPaths([string[]]$Paths) {
    # Check each ancestor once, before any write, including ancestors outside the repository.
    $seen = [Collections.Generic.HashSet[string]]::new([StringComparer]::OrdinalIgnoreCase)
    foreach ($path in $Paths) {
        $current = [IO.Path]::GetFullPath($path)
        while ($current -and $seen.Add($current)) {
            if (Test-Path -LiteralPath $current) {
                if ((Get-Item -LiteralPath $current -Force).Attributes -band [IO.FileAttributes]::ReparsePoint) {
                    throw "Reparse paths are unsupported: $current"
                }
            }
            $current = Split-Path $current -Parent
        }
    }
}

function Get-PBZBuildSources([string]$Root) {
    $sources = @('CMakeLists.txt', 'CMakePresets.json', 'cmake/PBZTooling.cmake', 'mods/CMakeLists.txt',
        'mods/PBZEditor/CMakeLists.txt', 'scripts/build.ps1', 'scripts/setup.ps1', 'scripts/profile.ps1',
        'scripts/runtime.ps1', '.clang-format')
    $mod = Join-Path $Root 'mods/PBZEditor'
    foreach ($file in Get-ChildItem -LiteralPath $mod -File -Recurse) {
        if ($file.Extension -in @('.cpp', '.hpp', '.h', '.inl')) {
            $sources += [IO.Path]::GetRelativePath($Root, $file.FullName).Replace('\', '/')
        }
    }
    $sources = @($sources | Sort-Object -Unique)
    Assert-PBZPaths @($sources | ForEach-Object { Join-Path $Root $_ })
    return $sources
}

function Get-PBZBuild([string]$Root, [string]$BuildDirectory) {
    if (-not $BuildDirectory) { $BuildDirectory = Join-Path $Root 'build/msvc' }
    if (-not [IO.Path]::IsPathRooted($BuildDirectory)) { $BuildDirectory = Join-Path $Root $BuildDirectory }
    $BuildDirectory = [IO.Path]::GetFullPath($BuildDirectory).TrimEnd('\', '/')
    $prefix = [IO.Path]::GetFullPath((Join-Path $Root 'build')) + [IO.Path]::DirectorySeparatorChar
    if (-not $BuildDirectory.StartsWith($prefix, [StringComparison]::OrdinalIgnoreCase)) { throw 'BuildDirectory must be inside repository build/.' }
    Assert-PBZPaths @($BuildDirectory)
    $profile = Get-PBZProfile $Root
    & pwsh -NoProfile -File (Join-Path $Root 'scripts/setup.ps1') -VerifyOnly | Out-Host
    if ($LASTEXITCODE -ne 0) { throw 'Source dependency verification failed.' }
    $info = Get-Content -LiteralPath (Join-Path $BuildDirectory 'build-info.json') -Raw | ConvertFrom-Json -AsHashtable
    if ($info.schemaVersion -ne 1 -or $info.purpose -cne 'PBZ-source-build' -or $info.status -cne 'success' -or
        $info.triplet -cne 'Game__Shipping__Win64' -or $info.ue4ssCommit -cne $profile.ue4ssCommit -or
        $info.profileManifestSha256 -cne $profile.profileManifestSha256 -or $info.configSha256 -cne $profile.configSha256 -or
        $info.toolchain.compiler -cne 'MSVC' -or $info.toolchain.architecture -cne 'x64') { throw 'Invalid or stale build receipt. Build again.' }
    $sources = @(Get-PBZBuildSources $Root)
    if (@(Compare-Object @($info.sourceFiles.Keys | Sort-Object) @($sources | Sort-Object)).Count) { throw 'Build source receipt is incomplete.' }
    foreach ($relative in $sources) {
        if ((Get-PBZHash (Join-Path $Root $relative)) -cne $info.sourceFiles[$relative]) { throw "Source changed since build: $relative" }
    }
    $nested = @(& git.exe -C $Root submodule status --recursive)
    if ($LASTEXITCODE -ne 0 -or ($nested -join "`n") -cne ($info.recursiveSubmodules -join "`n")) { throw 'Build source revisions changed.' }
    $expected = @('Game__Shipping__Win64/bin/UE4SS.dll', 'Game__Shipping__Win64/bin/dwmapi.dll', 'Game__Shipping__Win64/bin/PBZEditor.dll')
    if (@($info.artifacts).Count -ne 3 -or @(Compare-Object @($info.artifacts.path | Sort-Object) @($expected | Sort-Object)).Count) { throw 'Invalid artifact receipt.' }
    foreach ($item in $info.artifacts) {
        if ((Get-PBZHash (Join-Path $BuildDirectory $item.path)) -cne $item.sha256) { throw "Build artifact changed: $($item.path)" }
    }
    return @{ directory=$BuildDirectory; profile=$profile; info=$info }
}

function Get-PBZRuntimeFiles([string]$Root, [string]$BuildDirectory) {
    $bin = Join-Path $BuildDirectory 'Game__Shipping__Win64/bin'
    $files = [ordered]@{
        'dwmapi.dll' = (Join-Path $bin 'dwmapi.dll')
        'ue4ss/UE4SS.dll' = (Join-Path $bin 'UE4SS.dll')
        'ue4ss/Mods/PBZEditor/dlls/main.dll' = (Join-Path $bin 'PBZEditor.dll')
        'ue4ss/UE4SS-settings.ini' = (Join-Path $Root 'config/UE4SS-settings.ini')
        'ue4ss/MemberVariableLayout.ini' = (Join-Path $Root 'game-configs/PhantomBladeZero/MemberVariableLayout.ini')
        'ue4ss/UE4SS_Signatures/GNatives.lua' = (Join-Path $Root 'game-configs/PhantomBladeZero/UE4SS_Signatures/GNatives.lua')
        'ue4ss/UE4SS_Signatures/GUObjectArray.lua' = (Join-Path $Root 'game-configs/PhantomBladeZero/UE4SS_Signatures/GUObjectArray.lua')
        'ue4ss/Mods/Keybinds/Scripts/main.lua' = (Join-Path $Root 'RE-UE4SS/assets/Mods/Keybinds/Scripts/main.lua')
        'ue4ss/LICENSE-UE4SS.txt' = (Join-Path $Root 'RE-UE4SS/LICENSE')
    }
    return $files
}

function New-PBZRuntime([string]$Root, [string]$BuildDirectory) {
    $build = Get-PBZBuild $Root $BuildDirectory
    $files = Get-PBZRuntimeFiles $Root $build.directory
    $generated = @('ue4ss/Mods/mods.txt', 'ue4ss/Mods/PBZEditor/enabled.txt')
    $destination = Join-Path $Root 'build/runtime'
    $allowed = @($files.Keys) + $generated
    $paths = @($destination) + @($allowed | ForEach-Object { Join-Path $destination $_ }) + @($files.Values)
    if (Test-Path -LiteralPath $destination) {
        # Enumeration does not follow junctions; every discovered entry is checked below.
        $entries = @(Get-ChildItem -LiteralPath $destination -Recurse -Force)
        $paths += @($entries.FullName)
        foreach ($file in @($entries | Where-Object { -not $_.PSIsContainer })) {
            if ([IO.Path]::GetRelativePath($destination, $file.FullName).Replace('\', '/') -cnotin $allowed) { throw "Unexpected staging file preserved: $($file.FullName)" }
        }
    }
    Assert-PBZPaths $paths
    foreach ($relative in $allowed) {
        $target = Join-Path $destination $relative
        [IO.Directory]::CreateDirectory((Split-Path $target -Parent)) | Out-Null
        if ($files.Contains($relative)) { [IO.File]::Copy($files[$relative], $target, $true) }
        elseif ($relative -eq 'ue4ss/Mods/mods.txt') { [IO.File]::WriteAllText($target, "Keybinds : 1`n") }
        else { [IO.File]::WriteAllText($target, '') }
    }
    return $destination
}

# These OS boundaries are independently replaceable in test scope, never through CLI flags.
function Get-PBZGameVersion([string]$Executable) { return (Get-Item -LiteralPath $Executable).VersionInfo.FileVersion }
function Assert-PBZStopped {
    if (Get-Process -Name PBZ -ErrorAction SilentlyContinue) { throw 'Fully exit PBZ before delivery or restore.' }
}
function Copy-PBZFile([string]$Source, [string]$Destination) { [IO.File]::Copy($Source, $Destination, $true) }
function Assert-PBZFile([string]$Path, [string]$Hash) {
    if ((Get-PBZHash $Path) -cne $Hash) { throw "Readback mismatch: $Path" }
}

function Assert-PBZUnlocked([string[]]$Paths) {
    $handles = [Collections.Generic.List[IDisposable]]::new()
    try {
        foreach ($path in $Paths) {
            if (Test-Path -LiteralPath $path) {
                $handles.Add([IO.File]::Open($path, [IO.FileMode]::Open, [IO.FileAccess]::ReadWrite, [IO.FileShare]::None))
            }
        }
    } finally { foreach ($handle in $handles) { $handle.Dispose() } }
}

function Save-PBZDeliveryReceipt([hashtable]$Receipt, [string]$Path) {
    $Receipt | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath $Path -Encoding utf8NoBOM
}

function Restore-PBZFiles([hashtable]$Receipt, [string]$BackupDirectory, $Files, $Directories) {
    $errors = [Collections.Generic.List[string]]::new()
    foreach ($item in $Files) {
        $target = Join-Path $Receipt.destination $item.path
        try {
            if ($item.existed) {
                Copy-PBZFile (Join-Path $BackupDirectory $item.backup) $target
                Assert-PBZFile $target $item.originalHash
            } else {
                if (Test-Path -LiteralPath $target) { [IO.File]::Delete($target) }
                if (Test-Path -LiteralPath $target) { throw "Expected absent after restore: $target" }
            }
        } catch { $errors.Add("$($item.path): $($_.Exception.Message)") }
        try { $item.actualHash = if (Test-Path -LiteralPath $target) { Get-PBZHash $target } else { 'absent' } }
        catch { $item.actualHash = 'unknown' }
    }
    foreach ($directory in @($Directories | Sort-Object Length -Descending)) {
        try {
            if ((Test-Path -LiteralPath $directory) -and @(Get-ChildItem -LiteralPath $directory -Force).Count -eq 0) {
                [IO.Directory]::Delete($directory, $false)
            }
        } catch { $errors.Add("$directory`: $($_.Exception.Message)") }
    }
    $Receipt.restoreErrors = @($errors.ToArray())
    $Receipt.status = if ($errors.Count) { 'restore-failed' } else { 'restored' }
    return ($errors.Count -eq 0)
}

function Invoke-PBZDelivery {
    param([string]$Root, [string]$InstallDirectory, [string]$BuildDirectory, [switch]$InitialInstall, [string]$Restore)
    $Root = [IO.Path]::GetFullPath($Root).TrimEnd('\', '/')
    $backupRoot = Join-Path $Root 'build/deploy-backups'
    $profile = Get-PBZProfile $Root
    $receipt = $null
    if ($Restore) {
        $receiptPath = [IO.Path]::GetFullPath($Restore)
        $backupDirectory = Split-Path $receiptPath -Parent
        if ((Split-Path $backupDirectory -Parent) -ine $backupRoot -or (Split-Path $receiptPath -Leaf) -cne 'receipt.json') { throw 'Restore requires a receipt inside this repository build/deploy-backups/<id>/.' }
        Assert-PBZPaths @($receiptPath)
        $receipt = Get-Content -LiteralPath $receiptPath -Raw | ConvertFrom-Json -AsHashtable
        if ($receipt.schemaVersion -ne 1 -or $receipt.purpose -cne 'PBZ-delivery' -or $receipt.repository -ine $Root -or
            $receipt.mode -cnotin @('install', 'deploy')) { throw 'Invalid delivery receipt.' }
        if ($InstallDirectory -and [IO.Path]::GetFullPath($InstallDirectory).TrimEnd('\', '/') -ine $receipt.installDirectory) { throw 'InstallDirectory differs from restore receipt.' }
        $InstallDirectory = $receipt.installDirectory
        $InitialInstall = $receipt.mode -ceq 'install'
    }
    if (-not $InstallDirectory) { throw 'InstallDirectory is required (the game root containing platform/steam/PBZ.exe).' }
    $InstallDirectory = [IO.Path]::GetFullPath($InstallDirectory).TrimEnd('\', '/')
    $destination = Join-Path $InstallDirectory 'platform/steam'
    $executable = Join-Path $destination 'PBZ.exe'
    if ([IO.Path]::GetPathRoot($destination) -ine [IO.Path]::GetPathRoot($backupRoot)) { throw 'Game and repository backup directory must be on the same volume.' }
    if ($destination.StartsWith($Root + '\', [StringComparison]::OrdinalIgnoreCase) -and
        -not $InstallDirectory.StartsWith($Root + '\.qa-', [StringComparison]::OrdinalIgnoreCase)) { throw 'Game destination collides with the repository.' }
    if ($Root.StartsWith($InstallDirectory + '\', [StringComparison]::OrdinalIgnoreCase)) { throw 'Repository is inside game destination.' }
    $allFiles = Get-PBZRuntimeFiles $Root (Join-Path $Root 'build/msvc')
    $allowed = if ($InitialInstall) { @($allFiles.Keys) + @('ue4ss/Mods/mods.txt', 'ue4ss/Mods/PBZEditor/enabled.txt') }
        else { @('ue4ss/UE4SS.dll', 'ue4ss/Mods/PBZEditor/dlls/main.dll') }
    $targets = @($allowed | ForEach-Object { Join-Path $destination $_ })
    $paths = @($executable, $backupRoot, (Join-Path $destination 'ue4ss/Mods/PBZEditor/enabled.txt')) + $targets + @($profile.manifest.files.Keys | ForEach-Object { Join-Path $destination "ue4ss/$_" })
    if ($Restore) {
        if ($receipt.destination -ine $destination -or @($receipt.files).Count -ne $allowed.Count -or
            @(Compare-Object @($receipt.files.path | Sort-Object) @($allowed | Sort-Object)).Count) { throw 'Restore file allowlist mismatch.' }
        $index = 0
        foreach ($item in $receipt.files) {
            if ($item.path -isnot [string] -or $item.path -cne $allowed[$index] -or
                $item.backup -isnot [string] -or $item.backup -cne "$index.bin" -or
                $item.existed -isnot [bool] -or -not $item.Contains('originalHash') -or
                ($item.existed -and ($item.originalHash -isnot [string] -or $item.originalHash -cnotmatch '^[0-9a-f]{64}$')) -or
                (-not $item.existed -and (-not $InitialInstall -or $null -ne $item.originalHash))) { throw 'Invalid backup entry.' }
            $paths += Join-Path $backupDirectory $item.backup
            $index++
        }
        $possibleDirectories = @($targets | ForEach-Object {
            $parent = Split-Path $_ -Parent
            while ($parent.Length -ge $destination.Length) { $parent; $parent = Split-Path $parent -Parent }
        })
        foreach ($directory in $receipt.createdDirectories) {
            if ($directory -notin $possibleDirectories) { throw 'Invalid created directory receipt.' }
        }
        $paths += @($receiptPath) + @($receipt.createdDirectories)
    }
    Assert-PBZPaths $paths
    if ((Get-PBZGameVersion $executable) -cne $profile.manifest.gameFileVersion) { throw 'Unsupported PBZ executable FileVersion.' }
    Assert-PBZStopped
    Assert-PBZUnlocked $targets
    if ($Restore) {
        foreach ($item in $receipt.files) {
            $backup = Join-Path $backupDirectory $item.backup
            if ($item.existed) {
                if (-not (Test-Path -LiteralPath $backup -PathType Leaf)) { throw 'Missing original backup.' }
                Assert-PBZFile $backup $item.originalHash
            } elseif (Test-Path -LiteralPath $backup) { throw 'Unexpected backup for absent original.' }
        }
        $ok = Restore-PBZFiles $receipt $backupDirectory $receipt.files $receipt.createdDirectories
        Save-PBZDeliveryReceipt $receipt $receiptPath
        if (-not $ok) { throw "Recovery incomplete; backups retained: $receiptPath" }
        return $receiptPath
    }
    $build = Get-PBZBuild $Root $BuildDirectory
    foreach ($relative in $profile.manifest.files.Keys) {
        $installed = Join-Path $destination "ue4ss/$relative"
        if (-not $InitialInstall) {
            Assert-PBZFile $installed $profile.manifest.files[$relative]
        }
    }
    if (-not $InitialInstall) {
        foreach ($relative in @('ue4ss/UE4SS.dll', 'ue4ss/Mods/PBZEditor/dlls/main.dll', 'ue4ss/Mods/PBZEditor/enabled.txt')) {
            if (-not (Test-Path -LiteralPath (Join-Path $destination $relative) -PathType Leaf)) { throw 'Install the full runtime first, including the PBZEditor enable marker.' }
        }
    }
    $runtime = New-PBZRuntime $Root $build.directory
    $backupDirectory = Join-Path $backupRoot ([Guid]::NewGuid().ToString('N'))
    if (Test-Path -LiteralPath $backupDirectory) { throw 'Backup directory collision.' }
    $receiptPath = Join-Path $backupDirectory 'receipt.json'
    $receipt = @{
        schemaVersion=1; purpose='PBZ-delivery'; repository=$Root; installDirectory=$InstallDirectory
        destination=$destination; mode=$(if ($InitialInstall) { 'install' } else { 'deploy' })
        status='prepared'; createdUtc=[DateTime]::UtcNow.ToString('o'); files=@(); createdDirectories=@(); restoreErrors=@()
    }
    [IO.Directory]::CreateDirectory($backupDirectory) | Out-Null
    # Copy every original before the first replacement. Failed backup preparation never mutates the game.
    $index = 0
    foreach ($relative in $allowed) {
        $target = Join-Path $destination $relative
        $exists = Test-Path -LiteralPath $target
        if (-not $InitialInstall -and -not $exists) { throw 'Daily deployment requires both originals.' }
        $originalHash = if ($exists) { Get-PBZHash $target } else { $null }
        if ($exists) {
            [IO.File]::Copy($target, (Join-Path $backupDirectory "$index.bin"), $false)
            Assert-PBZFile (Join-Path $backupDirectory "$index.bin") $originalHash
        }
        $receipt.files += @{ path=$relative; backup="$index.bin"; existed=[bool]$exists; originalHash=$originalHash; installedHash=(Get-PBZHash (Join-Path $runtime $relative)); actualHash=$originalHash }
        $index++
        $parent = Split-Path $target -Parent
        while (-not (Test-Path -LiteralPath $parent)) {
            if ($parent -notin $receipt.createdDirectories) { $receipt.createdDirectories += $parent }
            $parent = Split-Path $parent -Parent
        }
    }
    Save-PBZDeliveryReceipt $receipt $receiptPath
    Assert-PBZStopped
    Assert-PBZUnlocked $targets
    $attempted = [Collections.Generic.List[object]]::new()
    $directories = [Collections.Generic.HashSet[string]]::new([StringComparer]::OrdinalIgnoreCase)
    try {
        foreach ($item in $receipt.files) {
            $target = Join-Path $destination $item.path
            $parent = Split-Path $target -Parent
            while ($parent -in $receipt.createdDirectories -and -not (Test-Path -LiteralPath $parent)) {
                $null = $directories.Add($parent)
                $parent = Split-Path $parent -Parent
            }
            [IO.Directory]::CreateDirectory((Split-Path $target -Parent)) | Out-Null
            $attempted.Add($item)
            Copy-PBZFile (Join-Path $runtime $item.path) $target
            Assert-PBZFile $target $item.installedHash
            $item.actualHash = $item.installedHash
        }
        $receipt.status = 'installed'
        Save-PBZDeliveryReceipt $receipt $receiptPath
    } catch {
        $failure = $_.Exception.Message
        $ok = Restore-PBZFiles $receipt $backupDirectory $attempted $directories
        Save-PBZDeliveryReceipt $receipt $receiptPath
        if (-not $ok) { throw "Delivery failed: $failure. Recovery incomplete; backups retained: $receiptPath" }
        throw "Delivery failed and original files restored: $failure. Receipt: $receiptPath"
    }
    return $receiptPath
}
