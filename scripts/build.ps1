#Requires -Version 7.0
[CmdletBinding()]
param(
    [string]$BuildDirectory,
    [int]$Jobs = 4,
    [switch]$ConfigureOnly,
    [switch]$PreflightOnly
)
$ErrorActionPreference = 'Stop'
function Initialize-BuildToolchain {
    $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio/Installer/vswhere.exe'
    if (-not (Test-Path -LiteralPath $vswhere)) { throw 'Visual Studio Installer/vswhere.exe is missing.' }
    $installation = & $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
    if ($LASTEXITCODE -ne 0 -or -not $installation) { throw 'Install Visual Studio Desktop development with C++.' }
    & (Join-Path $installation 'Common7/Tools/Launch-VsDevShell.ps1') -Arch amd64 -HostArch amd64 -SkipAutomaticLocation
    foreach ($name in @('cmake', 'ninja', 'ml64', 'link', 'rc', 'mt', 'cargo', 'rustc')) {
        if (-not (Get-Command $name -CommandType Application -ErrorAction SilentlyContinue)) { throw "Missing required tool: $name" }
    }
    $compiler = 'cl'
    if (-not (Get-Command $compiler -CommandType Application -ErrorAction SilentlyContinue)) { throw "Missing compiler: $compiler" }
    $cmakeVersion = & cmake --version
    if ($LASTEXITCODE -ne 0 -or $cmakeVersion[0] -notmatch 'cmake version (\d+\.\d+\.\d+)' -or [version]$Matches[1] -lt [version]'3.26.0') {
        throw 'CMake 3.26 or newer is required.'
    }
    if (-not $env:WindowsSdkDir -or -not $env:WindowsSDKVersion -or
        -not (Test-Path (Join-Path $env:WindowsSdkDir "Include/$($env:WindowsSDKVersion.TrimEnd('\'))/um/Windows.h"))) {
        throw 'Windows SDK headers are unavailable.'
    }
    if ($env:VSCMD_ARG_TGT_ARCH -ne 'x64') { throw 'Visual Studio did not activate the x64 target environment.' }
    Write-Host 'Toolchain ready: MSVC x64'
}

function Assert-BuildCache {
    param([string]$BuildDirectory)
    $cachePath = Join-Path $BuildDirectory 'CMakeCache.txt'
    if (-not (Test-Path -LiteralPath $cachePath)) { return }
    $cache = Get-Content -LiteralPath $cachePath -Raw
    $compiler = 'cl.exe'
    foreach ($language in @('C', 'CXX')) {
        $pattern = '(?m)^CMAKE_' + $language + '_COMPILER:[^=]+=(.+)\r?$'
        if ($cache -notmatch $pattern -or
            [IO.Path]::GetFileName($Matches[1].Trim()) -ine $compiler) {
            throw "Existing cache uses a different or unknown $language compiler. Choose a new BuildDirectory."
        }
    }
}

function Assert-MsvcDependencyPrefix {
    param([string]$BuildDirectory)
    $rules = Join-Path $BuildDirectory 'CMakeFiles/rules.ninja'
    $expectedFile = Join-Path $BuildDirectory 'CMakeFiles/PBZShowIncludes/prefix.bin'
    if (-not (Test-Path -LiteralPath $rules) -or -not (Test-Path -LiteralPath $expectedFile)) {
        throw 'MSVC include prefix has not been verified. Preserve this cache and choose a fresh BuildDirectory.'
    }
    # Latin1 maps each byte to one character without changing localized compiler output.
    $bytes = [Text.Encoding]::Latin1
    $expected = $bytes.GetString([IO.File]::ReadAllBytes($expectedFile))
    $prefixes = [regex]::Matches($bytes.GetString([IO.File]::ReadAllBytes($rules)), '(?m)^msvc_deps_prefix = ([^\r\n]*)')
    if (-not $expected -or $prefixes.Count -eq 0) { throw 'Missing MSVC dependency prefix.' }
    foreach ($prefix in $prefixes) {
        if ($prefix.Groups[1].Value -cne $expected) {
            throw 'Ninja include prefix differs from the compiler bytes. Preserve this cache and choose a fresh BuildDirectory.'
        }
    }
}
function Assert-ModHeaderDependencies {
    param([string]$BuildDirectory)
    foreach ($object in @(
        'mods/PBZEditor/CMakeFiles/PBZEditor.dir/src/dllmain.cpp.obj',
        'mods/PBZEditor/CMakeFiles/PBZEditor.dir/src/items/item_runtime.cpp.obj',
        'mods/PBZEditor/CMakeFiles/PBZEditor.dir/src/items/item_ui.cpp.obj',
        'mods/PBZEditor/CMakeFiles/PBZItemTests.dir/tests/item_tests.cpp.obj')) {
        $dependencies = @(& ninja -C $BuildDirectory -t deps $object 2>&1)
        if ($LASTEXITCODE -ne 0) { throw "Cannot verify header dependencies: $object" }
        $text = ($dependencies -join "`n").Replace('\', '/').ToLowerInvariant()
        if ($text -notmatch '#deps [1-9][0-9]*' -or
            -not $text.Contains('mods/pbzeditor/inc/items/item_editor.hpp')) {
            throw "Header dependency tracking failed: $object. No build receipt will be issued."
        }
        if ($object -match '(dllmain|item_runtime)\.cpp\.obj$' -and
            -not $text.Contains('mods/pbzeditor/src/items/item_runtime.hpp')) {
            throw "Runtime layout header is missing from dependencies: $object"
        }
    }
    Write-Host 'Verified MSVC header dependencies for mod and test consumers.'
}
function Invoke-BuildCommand {
    param([Parameter(Mandatory)][string]$Command, [string[]]$Arguments = @())
    $git = (Get-Command git.exe -CommandType Application -ErrorAction Stop | Select-Object -First 1).Source
    $gitRoot = Split-Path (Split-Path (Split-Path (& $git --exec-path) -Parent) -Parent) -Parent
    $bash = Join-Path $gitRoot 'bin/bash.exe'
    if (-not (Test-Path -LiteralPath $bash)) { throw 'Git for Windows Bash is required for CMake dependency operations.' }
    $link = (Get-Command link.exe -CommandType Application -ErrorAction Stop | Select-Object -First 1).Source
    if ($link -notmatch '[\\/]VC[\\/]Tools[\\/]MSVC[\\/]') {
        throw 'Activate the Visual Studio x64 toolchain before invoking build commands.'
    }
    # Preserve every VS/SDK tool directory and its ordering when crossing into MSYS.
    & $bash --noprofile --norc -c 'build_path=$(/usr/bin/cygpath -p -u "$1") || exit; shift; export PATH="$build_path:/mingw64/bin:/usr/bin"; export LC_ALL=C; exec "$@"' build-command $env:PATH $Command @Arguments
    if ($LASTEXITCODE -ne 0) { throw "$Command failed with exit code $LASTEXITCODE." }
}
function Get-BuildSourceHashes {
    param([string]$Root)
    $hashes = [ordered]@{}
    foreach ($relative in Get-PBZBuildSources $Root) {
        $hashes[$relative] = (Get-FileHash -LiteralPath (Join-Path $Root $relative)).Hash.ToLowerInvariant()
    }
    return $hashes
}
. (Join-Path $PSScriptRoot 'runtime.ps1')
$root = Split-Path $PSScriptRoot -Parent
$triplet = 'Game__Shipping__Win64'
if (-not $BuildDirectory) { $BuildDirectory = Join-Path $root 'build/msvc' }
if (-not [IO.Path]::IsPathRooted($BuildDirectory)) { $BuildDirectory = Join-Path $root $BuildDirectory }
$BuildDirectory = [IO.Path]::GetFullPath($BuildDirectory).TrimEnd('\', '/')
$buildRoot = [IO.Path]::GetFullPath((Join-Path $root 'build'))
if (-not $BuildDirectory.StartsWith($buildRoot + [IO.Path]::DirectorySeparatorChar, [StringComparison]::OrdinalIgnoreCase)) {
    throw 'BuildDirectory must be a child of this repository build directory.'
}
$checkPath = $BuildDirectory
while ($checkPath -and $checkPath.Length -ge $buildRoot.Length) {
    if ((Test-Path -LiteralPath $checkPath) -and ((Get-Item -LiteralPath $checkPath -Force).Attributes -band [IO.FileAttributes]::ReparsePoint)) {
        throw "Build path cannot contain a reparse point: $checkPath"
    }
    $checkPath = Split-Path $checkPath -Parent
}
$receipt = Join-Path $BuildDirectory 'build-info.json'
# Every invocation invalidates an earlier receipt, including preflight/configure-only failures.
if (Test-Path -LiteralPath $receipt) { Remove-Item -LiteralPath $receipt -Force }
try {
    if ($Jobs -lt 1 -or $Jobs -gt 64) { throw 'Jobs must be between 1 and 64.' }
    & pwsh -NoProfile -File (Join-Path $PSScriptRoot 'setup.ps1') -VerifyOnly
    if ($LASTEXITCODE -ne 0) { throw 'Dependency verification failed.' }
    $sourceHashes = Get-BuildSourceHashes -Root $root
    $inputs = Get-PBZProfile -Root $root
    $nested = @(& git -C $root submodule status --recursive)
    if ($LASTEXITCODE -ne 0) { throw 'Cannot read nested source revisions.' }
    $inputJson = $inputs | ConvertTo-Json -Depth 8 -Compress
    Initialize-BuildToolchain
    $compilerPath = (Get-Command cl.exe -CommandType Application | Select-Object -First 1).Source
    $compilerVersion = (Get-Item -LiteralPath $compilerPath).VersionInfo.FileVersion
    if ($compilerPath -notmatch '[\\/]VC[\\/]Tools[\\/]MSVC[\\/]' -or $compilerPath -notmatch '[\\/]Hostx64[\\/]x64[\\/]cl.exe$') { throw 'MSVC x64 compiler required; Clang is unsupported.' }
    $git = (Get-Command git.exe -CommandType Application | Select-Object -First 1).Source
    $bashPath = Join-Path (Split-Path (Split-Path (Split-Path (& $git --exec-path) -Parent) -Parent) -Parent) 'bin/bash.exe'
    if (-not (Test-Path -LiteralPath $bashPath)) { throw 'Git for Windows Bash is required.' }
    Assert-BuildCache -BuildDirectory $BuildDirectory
    if (Test-Path -LiteralPath (Join-Path $BuildDirectory 'mods/PBZEditor/CMakeFiles/PBZEditor.dir/src/dllmain.cpp.obj')) {
        Assert-MsvcDependencyPrefix -BuildDirectory $BuildDirectory
        Assert-ModHeaderDependencies -BuildDirectory $BuildDirectory
    }
    $cachePath = Join-Path $BuildDirectory 'CMakeCache.txt'
    if (Test-Path -LiteralPath $cachePath) {
        $cache = Get-Content -LiteralPath $cachePath -Raw
        foreach ($entry in @{
            CMAKE_HOME_DIRECTORY = $root.Replace('\', '/')
            CMAKE_BUILD_TYPE = $triplet
            CMAKE_GENERATOR = 'Ninja'
            PBZ_CONFIGURED_UE4SS_COMMIT = $inputs.ue4ssCommit
        }.GetEnumerator()) {
            $pattern = '(?m)^' + $entry.Key + ':[^=]+=(.+)\r?$'
            if ($cache -notmatch $pattern -or $Matches[1].Trim().Replace('\', '/') -cne $entry.Value) { throw "Foreign or incompatible cache: $($entry.Key). Choose a fresh BuildDirectory." }
        }
        foreach ($language in @('C', 'CXX')) {
            $pattern = '(?m)^CMAKE_' + $language + '_COMPILER:[^=]+=(.+)\r?$'
            if ($cache -notmatch $pattern) { throw 'Cache compiler is missing.' }
            $cachedCompiler = $Matches[1].Trim()
            if ($cachedCompiler -ieq 'cl.exe') {
                # The preset stores a command name; CMake records its resolved tool separately.
                $cacheVersion = @()
                foreach ($part in @('MAJOR', 'MINOR', 'PATCH')) {
                    if ($cache -notmatch "(?m)^CMAKE_CACHE_$($part)_VERSION:INTERNAL=(\d+)\r?$") { throw 'Cache CMake version is missing.' }
                    $cacheVersion += $Matches[1]
                }
                $compilerRecord = Join-Path $BuildDirectory "CMakeFiles/$($cacheVersion -join '.')/CMake${language}Compiler.cmake"
                $record = Get-Content -LiteralPath $compilerRecord -Raw
                $recordPattern = '(?m)^set\(CMAKE_' + $language + '_COMPILER "([^"]+)"\)'
                if ($record -notmatch $recordPattern) { throw 'Resolved cache compiler is missing.' }
                $cachedCompiler = $Matches[1]
            }
            if ([IO.Path]::GetFullPath($cachedCompiler) -ine [IO.Path]::GetFullPath($compilerPath)) { throw 'Cache compiler differs from selected Visual Studio installation.' }
        }
    }
    Write-Host "Verified source $($inputs.ue4ssCommit), MSVC $compilerVersion, SDK $env:WindowsSDKVersion"
    if ($PreflightOnly) { return }
    Push-Location $root
    try {
        $pwshPath = (Get-Command pwsh.exe -CommandType Application | Select-Object -First 1).Source
        Invoke-BuildCommand -Command 'cmake' -Arguments @('--preset', 'ninja-msvc-x64', '-B', $BuildDirectory, "-DPBZ_PWSH=$pwshPath", "-DPBZ_CONFIGURED_UE4SS_COMMIT=$($inputs.ue4ssCommit)")
        Assert-MsvcDependencyPrefix -BuildDirectory $BuildDirectory
        if ($ConfigureOnly) { return }
        Invoke-BuildCommand -Command 'cmake' -Arguments @('--build', $BuildDirectory, '--parallel', "$Jobs", '--target', 'format-check', 'UE4SS', 'proxy', 'PBZEditor', 'PBZItemTests')
        Assert-ModHeaderDependencies -BuildDirectory $BuildDirectory
        Invoke-BuildCommand -Command 'ctest' -Arguments @('--test-dir', $BuildDirectory, '--output-on-failure')
    } finally { Pop-Location }
    & pwsh -NoProfile -File (Join-Path $PSScriptRoot 'setup.ps1') -VerifyOnly
    if ($LASTEXITCODE -ne 0) { throw 'Source verification failed after build.' }
    if ((Get-BuildSourceHashes -Root $root | ConvertTo-Json -Depth 8 -Compress) -cne
        ($sourceHashes | ConvertTo-Json -Depth 8 -Compress)) { throw 'Project build sources changed during compilation.' }
    $after = Get-PBZProfile -Root $root
    $afterNested = @(& git -C $root submodule status --recursive)
    if ($LASTEXITCODE -ne 0 -or ($after | ConvertTo-Json -Depth 8 -Compress) -cne $inputJson -or
        ($nested -join "`n") -cne ($afterNested -join "`n")) { throw 'Build inputs changed during compilation.' }
    $artifacts = @()
    foreach ($name in @('UE4SS.dll', 'dwmapi.dll', 'PBZEditor.dll')) {
        $relative = "$triplet/bin/$name"
        $file = Join-Path $BuildDirectory $relative
        if (-not (Test-Path -LiteralPath $file) -or (Get-Item -LiteralPath $file).Length -eq 0) { throw "Missing build artifact: $relative" }
        $artifacts += [ordered]@{ path=$relative; sha256=(Get-FileHash -LiteralPath $file).Hash.ToLowerInvariant() }
    }
    $info = [ordered]@{
        schemaVersion=1; purpose='PBZ-source-build'; status='success'; completedUtc=[DateTime]::UtcNow.ToString('o')
        ue4ssCommit=$inputs.ue4ssCommit; recursiveSubmodules=$nested
        profileManifestSha256=$inputs.profileManifestSha256; configSha256=$inputs.configSha256
        triplet=$triplet; msvcDependencyPrefixSha256=(Get-FileHash -LiteralPath (Join-Path $BuildDirectory 'CMakeFiles/PBZShowIncludes/prefix.bin')).Hash.ToLowerInvariant(); headerDependencies='verified'
        toolchain=[ordered]@{ compiler='MSVC'; compilerPath=$compilerPath; compilerVersion=$compilerVersion; architecture='x64'; windowsSdk=$env:WindowsSDKVersion.TrimEnd('\'); cmake=(& cmake --version)[0]; ninja=(& ninja --version); rustc=(& rustc --version); cargo=(& cargo --version) }
        sourceFiles=$sourceHashes; artifacts=$artifacts
    }
    $info | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath $receipt -Encoding utf8NoBOM
    $runtime = New-PBZRuntime -Root $root -BuildDirectory $BuildDirectory
    Write-Host "Runtime assembled: $runtime"
    Write-Host "Build complete: $receipt"
} catch {
    if (Test-Path -LiteralPath $receipt) { Remove-Item -LiteralPath $receipt -Force }
    throw
}
