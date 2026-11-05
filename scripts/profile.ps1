# Shared by the build and explicit runtime packaging entry points.
function Get-PBZProfile {
    param([Parameter(Mandatory)][string]$Root)
    $profileRoot = Join-Path $Root 'game-configs/PhantomBladeZero'
    $manifestPath = Join-Path $profileRoot 'manifest.json'
    $manifest = Get-Content -LiteralPath $manifestPath -Raw | ConvertFrom-Json -AsHashtable
    $keys = @('schemaVersion', 'gameFileVersion', 'steamAppId', 'steamBuildId', 'testedUE4SSCommit', 'sourcePackage', 'files')
    if (@(Compare-Object @($manifest.Keys | Sort-Object) @($keys | Sort-Object)).Count -or
        $manifest.schemaVersion -ne 1 -or $manifest.gameFileVersion -ne 'CL-302256_JKS-17280' -or
        $manifest.steamAppId -ne 4115450 -or $manifest.steamBuildId -ne 26367565) { throw 'Unsupported PBZ profile schema or game baseline.' }
    if (@(Compare-Object @($manifest.sourcePackage.Keys | Sort-Object) @('sha256', 'url')).Count -or
        $manifest.sourcePackage.sha256 -cnotmatch '^[0-9a-f]{64}$' -or
        $manifest.sourcePackage.url -notmatch '^https://github.com/KintaroModding/ue4ss/releases/tag/') { throw 'Invalid profile source package.' }
    $gitlink = @(& git.exe -C $Root ls-files --stage -- RE-UE4SS)
    if ($LASTEXITCODE -ne 0 -or $gitlink.Count -ne 1 -or $gitlink[0] -notmatch '^160000 ([0-9a-f]{40}) 0\tRE-UE4SS$') { throw 'Missing UE4SS parent gitlink.' }
    $commit = $Matches[1]
    if ($manifest.testedUE4SSCommit -cne $commit) { throw 'Profile tested commit does not match parent gitlink.' }
    $files = @('MemberVariableLayout.ini', 'UE4SS_Signatures/GNatives.lua', 'UE4SS_Signatures/GUObjectArray.lua')
    if (@(Compare-Object @($manifest.files.Keys | Sort-Object) $files).Count) { throw 'Profile must contain exactly the three adaptation files.' }
    foreach ($relative in $files) {
        if ($manifest.files[$relative] -cnotmatch '^[0-9a-f]{64}$' -or
            (Get-FileHash -LiteralPath (Join-Path $profileRoot $relative) -Algorithm SHA256).Hash.ToLowerInvariant() -cne $manifest.files[$relative]) { throw "Profile hash mismatch: $relative" }
    }
    return [ordered]@{
        manifest = $manifest
        ue4ssCommit = $commit
        profileManifestSha256 = (Get-FileHash -LiteralPath $manifestPath).Hash.ToLowerInvariant()
        configSha256 = (Get-FileHash -LiteralPath (Join-Path $Root 'config/UE4SS-settings.ini')).Hash.ToLowerInvariant()
    }
}
