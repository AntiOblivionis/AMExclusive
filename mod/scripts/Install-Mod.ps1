[CmdletBinding()]
param(
    [string]$SourceDirectory = '',
    [string]$InstallDirectory = (Join-Path $env:PUBLIC 'AMExclusive'),
    [switch]$NoRestart,
    [switch]$EmitProgress,
    [ValidateSet('auto', 'zh-CN', 'en-US')][string]$Language = 'auto'
)

$ErrorActionPreference = 'Stop'
$script:ameChinese = $Language -eq 'zh-CN' -or ($Language -eq 'auto' -and
    [Globalization.CultureInfo]::CurrentUICulture.TwoLetterISOLanguageName -eq 'zh')
function Get-InstallerText([string]$Chinese, [string]$English) {
    if ($script:ameChinese) { return $Chinese }
    return $English
}
# Setup decodes captured output as UTF-8; preserve Chinese errors through the
# Windows PowerShell 5.1 child process as well as the native console.
[Console]::OutputEncoding = New-Object Text.UTF8Encoding($false)

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
            throw (Get-InstallerText "此目录中未找到发布文件：$resolved" "Release payload not found in: $resolved")
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
    throw (Get-InstallerText '找不到发布文件，请通过 -SourceDirectory 指定目录。' 'Could not locate a Release payload. Pass -SourceDirectory explicitly.')
}

function Stop-AppleMusicRuntime {
    foreach ($name in @('AppleMusic', 'AMPLibraryAgent', 'am-exclusive-broker')) {
        $processes = @(Get-Process $name -ErrorAction SilentlyContinue)
        $processes | Stop-Process -Force
        $processes | Wait-Process -Timeout 5 -ErrorAction Stop
        if (Get-Process $name -ErrorAction SilentlyContinue) {
            throw (Get-InstallerText "运行进程未停止：$name" "Runtime did not stop: $name")
        }
    }
}

function Start-AppleMusic([string]$Aumid) {
    if (-not $Aumid) { return }
    Start-Process -FilePath $env:WINDIR\explorer.exe -ArgumentList "shell:AppsFolder\$Aumid"
}

$source = Resolve-ReleaseDirectory $SourceDirectory
$install = [IO.Path]::GetFullPath($InstallDirectory)
$expectedInstall = [IO.Path]::GetFullPath((Join-Path $env:PUBLIC 'AMExclusive'))
if ($install -ne $expectedInstall) {
    throw (Get-InstallerText "拒绝使用非预期的安装目录：$install" "Refusing unexpected install directory: $install")
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
        throw (Get-InstallerText "缺少发布文件：$candidate" "Missing release artifact: $candidate")
    }
}
$installerScripts = @('Register-Companion.ps1', 'Uninstall-Mod.ps1', 'Uninstall.cmd')
foreach ($script in $installerScripts) {
    if (-not (Test-Path -LiteralPath (Join-Path $PSScriptRoot $script) -PathType Leaf)) {
        throw (Get-InstallerText "缺少安装支持文件：$script" "Missing installer support file: $script")
    }
}
$licenseSource = @(
    (Join-Path $PSScriptRoot 'LICENSE'),
    (Join-Path $PSScriptRoot '..\..\LICENSE')
) | Where-Object { Test-Path -LiteralPath $_ -PathType Leaf } | Select-Object -First 1
if (-not $licenseSource) { throw (Get-InstallerText '缺少 MIT LICENSE 许可证文件。' 'Missing MIT LICENSE file.') }
$thirdPartyNoticeSource = @(
    (Join-Path $PSScriptRoot 'THIRD_PARTY_NOTICES.md'),
    (Join-Path $PSScriptRoot '..\..\THIRD_PARTY_NOTICES.md')
) | Where-Object { Test-Path -LiteralPath $_ -PathType Leaf } | Select-Object -First 1
if (-not $thirdPartyNoticeSource) { throw (Get-InstallerText '缺少 THIRD_PARTY_NOTICES.md 文件。' 'Missing THIRD_PARTY_NOTICES.md file.') }

Write-ProgressMilestone 22 'validated'

# Capture source hashes before copying. A manifest derived only from the
# destination would bless a missing/corrupted/replaced payload as the release.
$expectedHashes = @{}
foreach ($name in $payloadFiles) { $expectedHashes[$name] = Get-Sha256Hex (Join-Path $source $name) }
foreach ($name in $installerScripts) { $expectedHashes[$name] = Get-Sha256Hex (Join-Path $PSScriptRoot $name) }
$expectedHashes['LICENSE'] = Get-Sha256Hex $licenseSource
$expectedHashes['THIRD_PARTY_NOTICES.md'] = Get-Sha256Hex $thirdPartyNoticeSource
$installedVersion = (Get-Item -LiteralPath (Join-Path $source 'am-exclusive-broker.exe')).VersionInfo.ProductVersion
if (-not $installedVersion) { throw (Get-InstallerText '伴生程序发布文件缺少产品版本信息。' 'Release broker is missing product version metadata.') }

$wasRunning = [bool](Get-Process AppleMusic -ErrorAction SilentlyContinue)
Stop-AppleMusicRuntime
Write-ProgressMilestone 32 'stop_runtime'

$preservedMode = $null
$preservedPeriod100ns = $null
$preservedAllowResampling = $null
foreach ($existingIni in @(
    (Join-Path $install 'am-exclusive.ini')
)) {
    if (-not (Test-Path -LiteralPath $existingIni -PathType Leaf)) { continue }
    foreach ($line in [IO.File]::ReadAllLines($existingIni)) {
        if ($line -match '^\s*mode\s*=\s*(probe|exclusive)\s*$') {
            $preservedMode = $Matches[1].ToLowerInvariant()
        }
        if ($line -match '^\s*allow_resampling\s*=\s*([01])\s*$') {
            $preservedAllowResampling = [int]$Matches[1]
        }
        if ($line -match '^\s*period_100ns\s*=\s*(\d+)\s*$') {
            [uint64]$candidatePeriod = 0
            if ([uint64]::TryParse($Matches[1], [ref]$candidatePeriod) -and
                $candidatePeriod -ge 10000 -and $candidatePeriod -le 1000000) {
                $preservedPeriod100ns = $candidatePeriod
            }
        }
    }
    if ($preservedMode -and $preservedPeriod100ns -and
        $null -ne $preservedAllowResampling) { break }
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
if ($LASTEXITCODE -ne 0) { throw (Get-InstallerText "设置安装目录权限失败：icacls 退出码 $LASTEXITCODE" "Failed to secure install directory: icacls exit $LASTEXITCODE") }
Write-ProgressMilestone 40 'secure_install'

foreach ($name in $payloadFiles) {
    $destination = Join-Path $install $name
    Copy-Item -LiteralPath (Join-Path $source $name) -Destination $destination -Force
    if ((Get-Sha256Hex $destination) -ne $expectedHashes[$name]) { throw (Get-InstallerText "发布文件验证失败：$name" "Payload verification failed: $name") }
}
if ($preservedMode) {
    $iniPath = Join-Path $install 'am-exclusive.ini'
    $iniText = [IO.File]::ReadAllText($iniPath)
    $iniText = [Text.RegularExpressions.Regex]::Replace(
        $iniText, '(?m)^\s*mode\s*=.*$', "mode=$preservedMode")
    $utf8NoBom = New-Object Text.UTF8Encoding($false)
    [IO.File]::WriteAllText($iniPath, $iniText, $utf8NoBom)
}
if ($preservedPeriod100ns) {
    $iniPath = Join-Path $install 'am-exclusive.ini'
    $iniText = [IO.File]::ReadAllText($iniPath)
    $iniText = [Text.RegularExpressions.Regex]::Replace(
        $iniText, '(?m)^\s*period_100ns\s*=.*$', "period_100ns=$preservedPeriod100ns")
    $utf8NoBom = New-Object Text.UTF8Encoding($false)
    [IO.File]::WriteAllText($iniPath, $iniText, $utf8NoBom)
}
if ($null -ne $preservedAllowResampling) {
    $iniPath = Join-Path $install 'am-exclusive.ini'
    $iniText = [IO.File]::ReadAllText($iniPath)
    $iniText = [Text.RegularExpressions.Regex]::Replace(
        $iniText, '(?m)^\s*allow_resampling\s*=.*$', "allow_resampling=$preservedAllowResampling")
    $utf8NoBom = New-Object Text.UTF8Encoding($false)
    [IO.File]::WriteAllText($iniPath, $iniText, $utf8NoBom)
}
# Only this file is intentionally transformed to preserve the user's settings.
$expectedHashes['am-exclusive.ini'] = Get-Sha256Hex (Join-Path $install 'am-exclusive.ini')
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
        SHA256 = $expectedHashes[$name]
    }
}
$manifest | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $install 'installed-manifest.json') -Encoding utf8
Write-ProgressMilestone 78 'write_manifest'

$registration = & (Join-Path $install 'Register-Companion.ps1') -InstallDirectory $install -Language $Language
Write-ProgressMilestone 90 'register_companion'

$uninstallKey = 'HKCU:\Software\Microsoft\Windows\CurrentVersion\Uninstall\AMExclusive'
New-Item -Path $uninstallKey -Force | Out-Null
$uninstallScript = Join-Path $install 'Uninstall-Mod.ps1'
$uninstallString = "powershell.exe -NoProfile -ExecutionPolicy Bypass -File `"$uninstallScript`""
Set-ItemProperty -Path $uninstallKey -Name DisplayName -Value 'AMExclusive'
Set-ItemProperty -Path $uninstallKey -Name DisplayVersion -Value $installedVersion
Set-ItemProperty -Path $uninstallKey -Name Publisher -Value 'AMExclusive'
Set-ItemProperty -Path $uninstallKey -Name InstallLocation -Value $install
Set-ItemProperty -Path $uninstallKey -Name DisplayIcon -Value (Join-Path $install 'am-exclusive-broker.exe')
Set-ItemProperty -Path $uninstallKey -Name UninstallString -Value $uninstallString
Set-ItemProperty -Path $uninstallKey -Name QuietUninstallString -Value ($uninstallString + ' -NoRestart')
New-ItemProperty -Path $uninstallKey -Name NoModify -Value 1 -PropertyType DWord -Force | Out-Null
New-ItemProperty -Path $uninstallKey -Name NoRepair -Value 1 -PropertyType DWord -Force | Out-Null
$estimatedKb = [int](($manifest | Measure-Object -Property Length -Sum).Sum / 1KB)
New-ItemProperty -Path $uninstallKey -Name EstimatedSize -Value $estimatedKb -PropertyType DWord -Force | Out-Null

Write-ProgressMilestone 94 'verify_result'
$savedManifest = ConvertFrom-Json -InputObject ([IO.File]::ReadAllText((Join-Path $install 'installed-manifest.json')))
if ($savedManifest.Count -ne $manifestFiles.Count) { throw (Get-InstallerText '安装清单验证失败。' 'Installed manifest verification failed.') }
foreach ($name in $manifestFiles) {
    $entries = @($savedManifest | Where-Object { $_.File -ceq $name })
    $item = Get-Item -LiteralPath (Join-Path $install $name)
    if ($entries.Count -ne 1 -or $entries[0].Length -ne $item.Length -or
        $entries[0].SHA256 -ne $expectedHashes[$name] -or
        (Get-Sha256Hex $item.FullName) -ne $expectedHashes[$name]) {
        throw (Get-InstallerText "最终安装文件验证失败：$name" "Final installed file verification failed: $name")
    }
}
$savedEntry = Get-ItemProperty -LiteralPath $uninstallKey
if ($savedEntry.DisplayName -ne 'AMExclusive' -or $savedEntry.DisplayVersion -ne $installedVersion -or
    $savedEntry.InstallLocation -ne $install -or $savedEntry.UninstallString -ne $uninstallString -or
    $savedEntry.QuietUninstallString -ne ($uninstallString + ' -NoRestart')) {
    throw (Get-InstallerText '卸载注册信息验证失败。' 'Uninstall registration verification failed.')
}

$restartRequested = $false
if ($wasRunning -and -not $NoRestart) {
    Start-AppleMusic $aumid
    $restartRequested = $true
}
Write-ProgressMilestone 96 'restart_apple_music'

[pscustomobject]@{
    Installed = $true
    Directory = $install
    AppleMusicVersion = $version
    CompanionTask = $registration.TaskName
    CompanionLifetime = $registration.Lifetime
    AutoStart = Get-InstallerText '仅随 Apple Music 启动，不随登录自动启动' 'Apple Music AppModel launch only; no login autostart'
    Uninstall = Get-InstallerText 'Windows 设置 > 应用 > 已安装的应用 > AMExclusive，或双击安装目录中的 Uninstall.cmd' 'Windows Settings > Apps > Installed apps > AMExclusive, or double-click Uninstall.cmd in the install folder'
    AppleMusicRestartRequested = $restartRequested
    Verified = $true
}
Write-ProgressMilestone 100 'complete'
