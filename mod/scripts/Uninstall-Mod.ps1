[CmdletBinding()]
param(
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
$install = [IO.Path]::GetFullPath((Join-Path $env:PUBLIC 'AMExclusive'))
$expected = [IO.Path]::GetFullPath((Join-Path $env:PUBLIC 'AMExclusive'))
if ($install -ne $expected -or -not $install.StartsWith([IO.Path]::GetFullPath($env:PUBLIC),
                                                        [StringComparison]::OrdinalIgnoreCase)) {
    throw (Get-InstallerText "拒绝使用不安全的卸载路径：$install" "Refusing unsafe uninstall path: $install")
}

function Get-SessionProcess([string]$Name) {
    # Only this logon session's processes; Get-Process spans every session.
    $sessionId = (Get-Process -Id $PID).SessionId
    return @(Get-Process $Name -ErrorAction SilentlyContinue |
        Where-Object { $_.SessionId -eq $sessionId })
}

function Stop-AppleMusicRuntime {
    Get-SessionProcess AppleMusic | Stop-Process -Force
    for ($i = 0; $i -lt 50 -and (Get-SessionProcess AppleMusic).Count -gt 0; $i++) {
        Start-Sleep -Milliseconds 100
    }
    Get-SessionProcess AMPLibraryAgent | Stop-Process -Force
    for ($i = 0; $i -lt 50 -and (Get-SessionProcess AMPLibraryAgent).Count -gt 0; $i++) {
        Start-Sleep -Milliseconds 100
    }
    Get-SessionProcess am-exclusive-broker | Stop-Process -Force
}

# Mirror of the installer's check: an uninstall under a different (elevated)
# account would delete the shared folder but leave the signed-in user's
# companion task and Apps entry behind while reporting success.
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
$package = Get-AppxPackage -Name AppleInc.AppleMusicWin -ErrorAction SilentlyContinue
$aumid = if ($package) { "$($package.PackageFamilyName)!App" } else { '' }
$wasRunning = [bool](Get-SessionProcess AppleMusic)
Stop-AppleMusicRuntime
Write-ProgressMilestone 30 'stop_runtime'

$sid = [Security.Principal.WindowsIdentity]::GetCurrent().User.Value
$taskName = "AMExclusive-Companion-$sid"
if (Get-ScheduledTask -TaskName $taskName -ErrorAction SilentlyContinue) {
    Unregister-ScheduledTask -TaskName $taskName -Confirm:$false
}
Remove-ItemProperty -Path 'HKCU:\Software\Microsoft\Windows\CurrentVersion\Run' `
                    -Name AMExclusive -ErrorAction SilentlyContinue
Remove-Item -LiteralPath 'HKCU:\Software\Microsoft\Windows\CurrentVersion\Uninstall\AMExclusive' `
            -Recurse -Force -ErrorAction SilentlyContinue
Write-ProgressMilestone 60 'remove_registration'

if (Test-Path -LiteralPath $install -PathType Container) {
    # A directory that is the current directory of this process (double-click
    # launch from the install folder, or an Apps-entry launch that inherited it)
    # cannot be removed; files would be deleted and the final RemoveDirectory
    # would fail, leaving an empty orphan folder and a failed uninstall.
    Set-Location -LiteralPath $env:SystemRoot
    [Environment]::CurrentDirectory = $env:SystemRoot
    Remove-Item -LiteralPath $install -Recurse -Force
}
Write-ProgressMilestone 85 'remove_directory'

$restarted = $false
if ($wasRunning -and -not $NoRestart -and $aumid) {
    Start-Process -FilePath $env:WINDIR\explorer.exe -ArgumentList "shell:AppsFolder\$aumid"
    $restarted = $true
}
Write-ProgressMilestone 95 'restart_apple_music'

$result = [pscustomobject]@{
    Uninstalled = $true
    ScheduledTaskRemoved = -not [bool](Get-ScheduledTask -TaskName $taskName -ErrorAction SilentlyContinue)
    LoginAutostartRemoved = -not [bool](Get-ItemProperty -Path 'HKCU:\Software\Microsoft\Windows\CurrentVersion\Run' -Name AMExclusive -ErrorAction SilentlyContinue)
    AppsEntryRemoved = -not (Test-Path -LiteralPath 'HKCU:\Software\Microsoft\Windows\CurrentVersion\Uninstall\AMExclusive')
    DirectoryRemoved = -not (Test-Path -LiteralPath $install)
    AppleMusicRestartRequested = $restarted
}
if (-not $result.ScheduledTaskRemoved -or -not $result.LoginAutostartRemoved -or
    -not $result.AppsEntryRemoved -or -not $result.DirectoryRemoved) {
    throw (Get-InstallerText '卸载验证失败：仍有注册项或文件残留。' 'Uninstall verification failed: registration or files remain.')
}
$result
Write-ProgressMilestone 100 'complete'
