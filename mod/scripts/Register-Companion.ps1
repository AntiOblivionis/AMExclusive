[CmdletBinding()]
param(
    [string]$InstallDirectory = (Join-Path $env:PUBLIC 'AMExclusive'),
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
$install = [IO.Path]::GetFullPath($InstallDirectory)
if ($install -ne [IO.Path]::GetFullPath((Join-Path $env:PUBLIC 'AMExclusive'))) {
    throw (Get-InstallerText "非预期的伴生程序目录：$install" "Unexpected companion directory: $install")
}
$broker = Join-Path $install 'am-exclusive-broker.exe'
if (-not (Test-Path -LiteralPath $broker)) { throw (Get-InstallerText '缺少伴生程序。' 'Broker is missing.') }
$sid = [Security.Principal.WindowsIdentity]::GetCurrent().User.Value
$taskName = "AMExclusive-Companion-$sid"
$service = New-Object -ComObject Schedule.Service
$service.Connect()
$folder = $service.GetFolder('\')
$task = $service.NewTask(0)
$task.RegistrationInfo.Description = Get-InstallerText '随 Apple Music 启动 AMExclusive。伴生程序绑定到一个 AppleMusic.exe 进程，并随该进程退出。' 'Start AMExclusive with Apple Music. The companion is bound to one AppleMusic.exe PID and exits when that PID exits.'
$task.Principal.UserId = $sid
$task.Principal.LogonType = 3
$task.Principal.RunLevel = 0
# TASK_INSTANCES_STOP_EXISTING: a new Apple Music launch replaces any stale companion
# instance instead of letting it adopt a different AppleMusic.exe lifetime.
$task.Settings.MultipleInstances = 3
$task.Settings.Hidden = $true
$task.Settings.AllowHardTerminate = $true
$task.Settings.DisallowStartIfOnBatteries = $false
$task.Settings.StopIfGoingOnBatteries = $false
$task.Settings.ExecutionTimeLimit = 'PT0S'
$task.Settings.StartWhenAvailable = $false
$task.Settings.AllowDemandStart = $true
$trigger = $task.Triggers.Create(0)
$trigger.Enabled = $true
$trigger.Subscription = @'
<QueryList><Query Id="0" Path="Microsoft-Windows-AppModel-Runtime/Admin"><Select Path="Microsoft-Windows-AppModel-Runtime/Admin">*[System[Provider[@Name='Microsoft-Windows-AppModel-Runtime'] and EventID=201] and EventData[Data[@Name='ImageName']='AppleMusic.exe' and Data[@Name='ApplicationName']='AppleInc.AppleMusicWin_nzyj5cx40ttqa!App']]</Select></Query></QueryList>
'@
$action = $task.Actions.Create(0)
$action.Path = $broker
$action.WorkingDirectory = $install
$registered = $folder.RegisterTaskDefinition($taskName, $task, 6, $sid, $null, 3, $null)
# Read back the persisted task, including the event and lifetime policy.
$registered = $folder.GetTask($taskName)
$definition = $registered.Definition
$registeredPrincipal = $definition.Principal.UserId
if ($registeredPrincipal -notmatch '^S-1-') {
    $registeredPrincipal = (New-Object Security.Principal.NTAccount($registeredPrincipal)).Translate(
        [Security.Principal.SecurityIdentifier]).Value
}
if (-not $registered.Enabled -or $definition.Actions.Count -ne 1 -or
    $definition.Actions.Item(1).Path -ne $broker -or
    $definition.Actions.Item(1).WorkingDirectory -ne $install -or
    -not [string]::IsNullOrEmpty($definition.Actions.Item(1).Arguments) -or
    $definition.Settings.MultipleInstances -ne 3 -or
    $definition.Settings.ExecutionTimeLimit -ne 'PT0S' -or
    $registeredPrincipal -ne $sid -or
    $definition.Principal.LogonType -ne 3 -or $definition.Principal.RunLevel -ne 0 -or
    $definition.Triggers.Count -ne 1 -or $definition.Triggers.Item(1).Type -ne 0 -or
    -not $definition.Triggers.Item(1).Enabled -or
    ([xml]$definition.Triggers.Item(1).Subscription).OuterXml -ne ([xml]$trigger.Subscription).OuterXml) {
    throw (Get-InstallerText '伴生计划任务验证失败。' 'Companion task verification failed.')
}
$runKey = 'HKCU:\Software\Microsoft\Windows\CurrentVersion\Run'
if (Test-Path -LiteralPath $runKey) {
    $runValues = Get-ItemProperty -LiteralPath $runKey -ErrorAction Stop
    if ($runValues.PSObject.Properties.Name -contains 'AMExclusive') {
        Remove-ItemProperty -LiteralPath $runKey -Name AMExclusive -ErrorAction Stop
    }
    if ((Get-ItemProperty -LiteralPath $runKey -ErrorAction Stop).PSObject.Properties.Name -contains 'AMExclusive') {
        throw (Get-InstallerText '无法删除旧版登录自启动项。' 'Could not remove the legacy login autostart entry.')
    }
}
[pscustomobject]@{
    TaskName = $taskName
    Enabled = $registered.Enabled
    Command = $broker
    Trigger = 'AppModel-Runtime/Admin 201'
    Lifetime = Get-InstallerText '严格绑定 AppleMusic.exe 进程' 'Strict AppleMusic.exe PID-bound'
    MultipleInstances = Get-InstallerText '停止现有实例' 'StopExisting'
    LoginAutostartRemoved = $true
}
