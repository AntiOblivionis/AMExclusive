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

$package = Get-AppxPackage -Name AppleInc.AppleMusicWin -ErrorAction SilentlyContinue
$aumid = if ($package) { "$($package.PackageFamilyName)!App" } else { '' }
$wasRunning = [bool](Get-Process AppleMusic -ErrorAction SilentlyContinue)
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
