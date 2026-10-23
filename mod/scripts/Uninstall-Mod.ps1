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
$currentSid = [Security.Principal.WindowsIdentity]::GetCurrent().User.Value
$shellSessionId = (Get-Process -Id $PID).SessionId
$shell = Get-CimInstance Win32_Process -Filter "Name = 'explorer.exe' AND SessionId = $shellSessionId" -ErrorAction SilentlyContinue |
    Select-Object -First 1
if ($shell) {
    $shellOwner = Invoke-CimMethod -InputObject $shell -MethodName GetOwnerSid -ErrorAction SilentlyContinue
    if ($shellOwner -and $shellOwner.ReturnValue -eq 0 -and $shellOwner.Sid -and $shellOwner.Sid -ne $currentSid) {
        throw (Get-InstallerText 'Uninstall is running under a different account than the signed-in user (administrator elevation). Run it without "Run as administrator" from the account that installed AMExclusive.' 'Uninstall is running under a different account than the signed-in user (administrator elevation). Run it without "Run as administrator" from the account that installed AMExclusive.')
    }
}

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
