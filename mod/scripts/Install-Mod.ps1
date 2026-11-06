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

# Every process that can hold one of the payload files open. Apple's own
# processes are session-scoped - Get-Process spans every logon session, and
# another user's player must not be killed. Our helpers have unique names, so
# a stray instance in any session is ours and would still lock the files.
function Get-AmeRuntimeProcesses {
    $sessionId = (Get-Process -Id $PID).SessionId
    $apple = @(Get-Process -Name 'AppleMusic', 'AMPLibraryAgent' -ErrorAction SilentlyContinue |
        Where-Object { $_.SessionId -eq $sessionId })
    $ours = @(Get-Process -Name 'am-exclusive-broker', 'am-exclusive-audiohost',
                                 'am_exclusive_media_control' -ErrorAction SilentlyContinue)
    return @($apple) + @($ours)
}

function Stop-AppleMusicRuntime {
    # The broker owns the audiohost watchdog and respawns the host about five
    # seconds after it dies. Stop Apple Music first, then the broker, then the
    # host/helper - and re-check, because a watchdog that was already pending
    # can bring the host back once more.
    $order = @{
        'AppleMusic' = 0
        'AMPLibraryAgent' = 0
        'am-exclusive-broker' = 1
        'am-exclusive-audiohost' = 2
        'am_exclusive_media_control' = 2
    }
    for ($attempt = 1; $attempt -le 3; $attempt++) {
        foreach ($process in @(Get-AmeRuntimeProcesses |
                Sort-Object { $order[$_.ProcessName] })) {
            Stop-Process -Id $process.Id -Force -ErrorAction SilentlyContinue
        }
        for ($tick = 0; $tick -lt 25; $tick++) {
            if (@(Get-AmeRuntimeProcesses).Count -eq 0) { return }
            Start-Sleep -Milliseconds 200
        }
    }
    $remaining = @(Get-AmeRuntimeProcesses |
        ForEach-Object { "$($_.ProcessName)($($_.Id))" })
    if ($remaining.Count -gt 0) {
        throw (Get-InstallerText `
            "无法关闭正在运行的程序：$($remaining -join ', ')。请退出 Apple Music 及其后台进程后重新运行安装程序。" `
            "Unable to stop running processes: $($remaining -join ', '). Close Apple Music and its background processes, then run the installer again.")
    }
}

function Start-AppleMusic([string]$Aumid) {
    if (-not $Aumid) { return }
    Start-Process -FilePath $env:WINDIR\explorer.exe -ArgumentList "shell:AppsFolder\$Aumid"
}

# Everything this script registers (directory ACL, companion task, Apps entry)
# is keyed to the token running it. Under over-the-shoulder UAC an elevated
# token belongs to the administrator who approved the prompt, not to the
# signed-in user who will run Apple Music; the install would then silently
# target the wrong account. Compare against the owner of this session's shell.
function Assert-SessionUser {
    $currentSid = [Security.Principal.WindowsIdentity]::GetCurrent().User.Value
    $shellSessionId = (Get-Process -Id $PID).SessionId
    $unknown = Get-InstallerText '无法验证当前桌面用户。请从使用 Apple Music 的用户桌面重试；安装和卸载尚未修改任何文件或注册。' 'Cannot verify the current desktop user. Retry from the desktop of the account using Apple Music; no installation files or registrations have been changed.'
    try {
        $shells = @(Get-CimInstance Win32_Process -Filter "Name = 'explorer.exe' AND SessionId = $shellSessionId" -ErrorAction Stop)
        if ($shells.Count -eq 0) { throw $unknown }
        foreach ($shell in $shells) {
            $owner = Invoke-CimMethod -InputObject $shell -MethodName GetOwnerSid -ErrorAction Stop
            if (-not $owner -or $owner.ReturnValue -ne 0 -or -not $owner.Sid) { throw $unknown }
            $ownerSid = (New-Object Security.Principal.SecurityIdentifier -ArgumentList $owner.Sid).Value
            if ($ownerSid -ne $currentSid) {
                throw (Get-InstallerText '运行账户与当前桌面用户不同。请从使用 Apple Music 的账户直接运行，不要使用其他账户提升权限。' 'The running account differs from the desktop user. Run directly from the account using Apple Music without elevation to a different account.')
            }
        }
    } catch {
        if ($_.Exception.Message -match '运行账户|running account') { throw }
        throw $unknown
    }
}
Assert-SessionUser
# Clear the runtime before anything else. A loaded hook DLL or a live
# audiohost locks its file and makes Copy-Item fail with "in use", which used
# to force a reboot and a second install attempt. The user never has to close
# Apple Music (or our helpers) by hand anymore.
$wasRunning = [bool](Get-Process AppleMusic -ErrorAction SilentlyContinue |
    Where-Object { $_.SessionId -eq (Get-Process -Id $PID).SessionId })
Stop-AppleMusicRuntime
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
    'am-exclusive-audiohost.exe',
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

Write-ProgressMilestone 32 'stop_runtime'

$preservedMode = $null
$preservedPeriod100ns = $null
$preservedAllowResampling = $null
$preservedOutput = $null
$preservedDlna = @{}
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
        if ($line -match '^\s*output\s*=\s*(wasapi|dlna)\s*$') {
            $preservedOutput = $Matches[1].ToLowerInvariant()
        }
        if ($line -match '^\s*(device_udn|device_name|location|buffer_ms|format_pref|max_bits|manual_urls)\s*=\s*(.*?)\s*$') {
            $preservedDlna[$Matches[1]] = $Matches[2]
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

# Keep the user's ini. The template is copied for hash verification, then the
# previous file is restored and only genuinely new keys are added - otherwise
# every upgrade resets device choices, [local] tuning and player mode.
$iniPath = Join-Path $install 'am-exclusive.ini'
$previousIni = if (Test-Path -LiteralPath $iniPath -PathType Leaf) {
    [IO.File]::ReadAllBytes($iniPath)
} else { $null }

foreach ($name in $payloadFiles) {
    $destination = Join-Path $install $name
    $copied = $false
    for ($attempt = 1; $attempt -le 3 -and -not $copied; $attempt++) {
        try {
            Copy-Item -LiteralPath (Join-Path $source $name) -Destination $destination -Force -ErrorAction Stop
            $copied = $true
        } catch {
            if ($attempt -eq 3) {
                throw (Get-InstallerText `
                    "文件被占用，重新停止后台进程后仍无法覆盖：$name。请重启电脑后再运行安装程序。" `
                    "File is still locked after stopping background processes again: $name. Restart Windows and run the installer again.")
            }
            # A companion task or the audiohost watchdog may have respawned a
            # helper between the initial stop and this copy. Clear and retry
            # instead of making the user reboot and start over.
            Stop-AppleMusicRuntime
            Start-Sleep -Milliseconds 500
        }
    }
    if ((Get-Sha256Hex $destination) -ne $expectedHashes[$name]) { throw (Get-InstallerText "发布文件验证失败：$name" "Payload verification failed: $name") }
}

if ($null -ne $previousIni) {
    [IO.File]::WriteAllBytes($iniPath, $previousIni)
    $existing = [IO.File]::ReadAllText($iniPath)
    $template = [IO.File]::ReadAllText((Join-Path $source 'am-exclusive.ini'))
    $rewritten = $false
    if ($existing -notmatch '(?m)^\s*player_mode\s*=') {
        $existing = [regex]::Replace($existing, '(?m)(^\[mod\][^\[]*)', "`$1player_mode=am`r`n")
        $rewritten = $true
    }
    if ($existing -notmatch '(?m)^\s*\[local\]') {
        $localSection = [regex]::Match($template, '(?s)\[local\].*?(?=\[dlna\])')
        if ($localSection.Success) {
            $existing = $existing.TrimEnd() + "`r`n`r`n" + $localSection.Value.TrimEnd() + "`r`n"
            $rewritten = $true
        }
    }
    if ($rewritten) {
        [IO.File]::WriteAllText($iniPath, $existing, (New-Object Text.UTF8Encoding($false)))
    }
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
if ($preservedOutput -or $preservedDlna.Count -gt 0) {
    $iniPath = Join-Path $install 'am-exclusive.ini'
    $iniText = [IO.File]::ReadAllText($iniPath)
    if ($preservedOutput) {
        $iniText = [Text.RegularExpressions.Regex]::Replace(
            $iniText, '(?m)^\s*output\s*=.*$', "output=$preservedOutput")
    }
    foreach ($key in $preservedDlna.Keys) {
        $value = $preservedDlna[$key]
        if ($key -eq 'buffer_ms') {
            [int]$buffer = 1500
            if ([int]::TryParse($value, [ref]$buffer)) {
                $buffer = [Math]::Min([Math]::Max($buffer, 300), 3000)
                $value = "$buffer"
            } else {
                $value = '1500'
            }
        }
        $iniText = [Text.RegularExpressions.Regex]::Replace(
            $iniText, "(?m)^\s*$key\s*=.*$", "$key=$value")
    }
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
