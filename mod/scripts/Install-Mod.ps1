[CmdletBinding()]
param(
    [string]$SourceDirectory = '',
    [string]$InstallDirectory = (Join-Path $env:PUBLIC 'AMExclusive'),
    [switch]$NoRestart,
    [switch]$EmitProgress
)

$ErrorActionPreference = 'Stop'

function Write-ProgressMilestone([int]$Percent, [string]$Message) {
    if ($EmitProgress) {
        $line = "AMMOD_PROGRESS|$Percent|$Message`r`n"
        $bytes = [Text.Encoding]::ASCII.GetBytes($line)
        $stream = [Console]::OpenStandardError()
        $stream.Write($bytes, 0, $bytes.Length)
        $stream.Flush()
    }
}

Write-ProgressMilestone 12 'validate'

function Get-Sha256Hex([string]$Path) {
    $stream = [IO.File]::OpenRead($Path)
    try {
        $sha = [Security.Cryptography.SHA256]::Create()
        try { return -join ($sha.ComputeHash($stream) | ForEach-Object { $_.ToString('X2') }) }
        finally { $sha.Dispose() }
    }
    finally { $stream.Dispose() }
}

function Resolve-ReleaseDirectory([string]$Requested) {
    if ($Requested) {
        $resolved = [IO.Path]::GetFullPath($Requested)
        if (-not (Test-Path -LiteralPath (Join-Path $resolved 'am-exclusive-broker.exe') -PathType Leaf)) {
            throw "Release payload not found in: $resolved"
        }
        return $resolved
    }

    $candidates = @(
        $PSScriptRoot,
        (Join-Path $PSScriptRoot '..\build-v2-native-passthrough-gapless-reset\Release'),
        (Join-Path $PSScriptRoot '..\build\Release'),
        (Join-Path $PSScriptRoot '..\Release')
    )
    foreach ($candidate in $candidates) {
        $resolved = [IO.Path]::GetFullPath($candidate)
        if (Test-Path -LiteralPath (Join-Path $resolved 'am-exclusive-broker.exe') -PathType Leaf) {
            return $resolved
        }
    }
    throw 'Could not locate a Release payload. Pass -SourceDirectory explicitly.'
}

function Stop-AppleMusicRuntime {
    Get-Process AppleMusic -ErrorAction SilentlyContinue | Stop-Process -Force
    for ($i = 0; $i -lt 50 -and (Get-Process AppleMusic -ErrorAction SilentlyContinue); $i++) {
        Start-Sleep -Milliseconds 100
    }
    Get-Process AMPLibraryAgent -ErrorAction SilentlyContinue | Stop-Process -Force
    for ($i = 0; $i -lt 50 -and (Get-Process AMPLibraryAgent -ErrorAction SilentlyContinue); $i++) {
        Start-Sleep -Milliseconds 100
    }
    Get-Process am-exclusive-broker -ErrorAction SilentlyContinue | Stop-Process -Force
}

function Start-AppleMusic([string]$Aumid) {
    if (-not $Aumid) { return }
    Start-Process -FilePath $env:WINDIR\explorer.exe -ArgumentList "shell:AppsFolder\$Aumid"
}

$source = Resolve-ReleaseDirectory $SourceDirectory
$install = [IO.Path]::GetFullPath($InstallDirectory)
$expectedInstall = [IO.Path]::GetFullPath((Join-Path $env:PUBLIC 'AMExclusive'))
if ($install -ne $expectedInstall) {
    throw "Refusing unexpected install directory: $install"
}
$package = Get-AppxPackage -Name AppleInc.AppleMusicWin -ErrorAction Stop
$appleMusic = Join-Path $package.InstallLocation 'AppleMusic.exe'
$version = (Get-Item -LiteralPath $appleMusic).VersionInfo.FileVersion
$aumid = "$($package.PackageFamilyName)!App"

$payloadFiles = @(
    'am-exclusive-broker.exe',
    'am_exclusive_media_control.exe',
    'am-exclusive-hook.dll',
    'am-exclusive-ui.dll',
    'am-exclusive.ini'
)
foreach ($name in $payloadFiles) {
    $candidate = Join-Path $source $name
    if (-not (Test-Path -LiteralPath $candidate -PathType Leaf)) {
        throw "Missing release artifact: $candidate"
    }
}
$installerScripts = @('Register-Companion.ps1', 'Uninstall-Mod.ps1', 'Uninstall.cmd')
foreach ($script in $installerScripts) {
    if (-not (Test-Path -LiteralPath (Join-Path $PSScriptRoot $script) -PathType Leaf)) {
        throw "Missing installer support file: $script"
    }
}
$licenseSource = @(
    (Join-Path $PSScriptRoot 'LICENSE'),
    (Join-Path $PSScriptRoot '..\..\LICENSE')
) | Where-Object { Test-Path -LiteralPath $_ -PathType Leaf } | Select-Object -First 1
if (-not $licenseSource) { throw 'Missing MIT LICENSE file.' }
$thirdPartyNoticeSource = @(
    (Join-Path $PSScriptRoot 'THIRD_PARTY_NOTICES.md'),
    (Join-Path $PSScriptRoot '..\..\THIRD_PARTY_NOTICES.md')
) | Where-Object { Test-Path -LiteralPath $_ -PathType Leaf } | Select-Object -First 1
if (-not $thirdPartyNoticeSource) { throw 'Missing THIRD_PARTY_NOTICES.md file.' }

Write-ProgressMilestone 22 'validated'

$wasRunning = [bool](Get-Process AppleMusic -ErrorAction SilentlyContinue)
Stop-AppleMusicRuntime
Write-ProgressMilestone 32 'stop_runtime'

$preservedMode = $null
foreach ($existingIni in @(
    (Join-Path $install 'am-exclusive.ini')
)) {
    if (-not (Test-Path -LiteralPath $existingIni -PathType Leaf)) { continue }
    foreach ($line in [IO.File]::ReadAllLines($existingIni)) {
        if ($line -match '^\s*mode\s*=\s*(probe|exclusive)\s*$') {
            $preservedMode = $Matches[1].ToLowerInvariant()
            break
        }
    }
    if ($preservedMode) { break }
}

if (Test-Path -LiteralPath $install -PathType Container) {
    foreach ($obsolete in @('am-exclusive-controller.exe', 'am-exclusive-pcm-probe.dll')) {
        Remove-Item -LiteralPath (Join-Path $install $obsolete) -Force -ErrorAction SilentlyContinue
    }
    Remove-Item -LiteralPath (Join-Path $install 'v2-final-dump') -Recurse -Force -ErrorAction SilentlyContinue
}

New-Item -ItemType Directory -Path $install -Force | Out-Null
$userSid = [Security.Principal.WindowsIdentity]::GetCurrent().User
& icacls.exe $install /inheritance:r /grant:r `
    "*$($userSid.Value):(OI)(CI)(F)" `
    '*S-1-5-18:(OI)(CI)(F)' `
    '*S-1-15-2-1:(OI)(CI)(RX)' | Out-Null
if ($LASTEXITCODE -ne 0) { throw "Failed to secure install directory: icacls exit $LASTEXITCODE" }
Write-ProgressMilestone 40 'secure_install'

foreach ($name in $payloadFiles) {
    $destination = Join-Path $install $name
    Copy-Item -LiteralPath (Join-Path $source $name) -Destination $destination -Force
}
if ($preservedMode) {
    $iniPath = Join-Path $install 'am-exclusive.ini'
    $iniText = [IO.File]::ReadAllText($iniPath)
    $iniText = [Text.RegularExpressions.Regex]::Replace(
        $iniText, '(?m)^\s*mode\s*=.*$', "mode=$preservedMode")
    $utf8NoBom = New-Object Text.UTF8Encoding($false)
    [IO.File]::WriteAllText($iniPath, $iniText, $utf8NoBom)
}
foreach ($script in $installerScripts) {
    Copy-Item -LiteralPath (Join-Path $PSScriptRoot $script) -Destination (Join-Path $install $script) -Force
}
Copy-Item -LiteralPath $licenseSource -Destination (Join-Path $install 'LICENSE') -Force
Copy-Item -LiteralPath $thirdPartyNoticeSource -Destination (Join-Path $install 'THIRD_PARTY_NOTICES.md') -Force
Write-ProgressMilestone 65 'copy_payload'

$manifestFiles = $payloadFiles + $installerScripts + @('LICENSE', 'THIRD_PARTY_NOTICES.md')
$manifest = foreach ($name in $manifestFiles) {
    $item = Get-Item -LiteralPath (Join-Path $install $name)
    [pscustomobject]@{
        File = $name
        Length = $item.Length
        SHA256 = Get-Sha256Hex $item.FullName
    }
}
$manifest | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $install 'installed-manifest.json') -Encoding utf8
Write-ProgressMilestone 78 'write_manifest'

$registration = & (Join-Path $install 'Register-Companion.ps1') -InstallDirectory $install
Write-ProgressMilestone 90 'register_companion'

$uninstallKey = 'HKCU:\Software\Microsoft\Windows\CurrentVersion\Uninstall\AMExclusive'
New-Item -Path $uninstallKey -Force | Out-Null
$uninstallScript = Join-Path $install 'Uninstall-Mod.ps1'
$uninstallString = "powershell.exe -NoProfile -ExecutionPolicy Bypass -File `"$uninstallScript`""
Set-ItemProperty -Path $uninstallKey -Name DisplayName -Value 'AMExclusive'
Set-ItemProperty -Path $uninstallKey -Name DisplayVersion -Value '2026.09.08'
Set-ItemProperty -Path $uninstallKey -Name Publisher -Value 'AMExclusive'
Set-ItemProperty -Path $uninstallKey -Name InstallLocation -Value $install
Set-ItemProperty -Path $uninstallKey -Name DisplayIcon -Value (Join-Path $install 'am-exclusive-broker.exe')
Set-ItemProperty -Path $uninstallKey -Name UninstallString -Value $uninstallString
Set-ItemProperty -Path $uninstallKey -Name QuietUninstallString -Value ($uninstallString + ' -NoRestart')
New-ItemProperty -Path $uninstallKey -Name NoModify -Value 1 -PropertyType DWord -Force | Out-Null
New-ItemProperty -Path $uninstallKey -Name NoRepair -Value 1 -PropertyType DWord -Force | Out-Null
$estimatedKb = [int](($manifest | Measure-Object -Property Length -Sum).Sum / 1KB)
New-ItemProperty -Path $uninstallKey -Name EstimatedSize -Value $estimatedKb -PropertyType DWord -Force | Out-Null

$restarted = $false
if ($wasRunning -and -not $NoRestart) {
    Start-AppleMusic $aumid
    $restarted = $true
}
Write-ProgressMilestone 96 'restart_apple_music'

[pscustomobject]@{
    Installed = $true
    Directory = $install
    AppleMusicVersion = $version
    CompanionTask = $registration.TaskName
    CompanionLifetime = $registration.Lifetime
    AutoStart = 'Apple Music AppModel launch only; no login autostart'
    Uninstall = 'Windows Settings > Apps > Installed apps > AMExclusive, or double-click Uninstall.cmd in the install folder'
    RestartedAppleMusic = $restarted
}
Write-ProgressMilestone 100 'complete'
