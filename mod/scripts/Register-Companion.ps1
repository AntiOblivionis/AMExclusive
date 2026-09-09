[CmdletBinding()]
param([string]$InstallDirectory = (Join-Path $env:PUBLIC 'AMExclusive'))
$ErrorActionPreference = 'Stop'
$install = [IO.Path]::GetFullPath($InstallDirectory)
if ($install -ne [IO.Path]::GetFullPath((Join-Path $env:PUBLIC 'AMExclusive'))) {
    throw "Unexpected companion directory: $install"
}
$broker = Join-Path $install 'am-exclusive-broker.exe'
if (-not (Test-Path -LiteralPath $broker)) { throw 'Broker is missing.' }
$sid = [Security.Principal.WindowsIdentity]::GetCurrent().User.Value
$taskName = "AMExclusive-Companion-$sid"
$service = New-Object -ComObject Schedule.Service
$service.Connect()
$folder = $service.GetFolder('\')
$task = $service.NewTask(0)
$task.RegistrationInfo.Description = 'Start AMExclusive with Apple Music. The companion is bound to one AppleMusic.exe PID and exits when that PID exits.'
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
if (-not $registered.Enabled -or $registered.Definition.Actions.Item(1).Path -ne $broker) {
    throw 'Companion task verification failed.'
}
$runKey = 'HKCU:\Software\Microsoft\Windows\CurrentVersion\Run'
Remove-ItemProperty -Path $runKey -Name AMExclusive -ErrorAction SilentlyContinue
[pscustomobject]@{
    TaskName = $taskName
    Enabled = $registered.Enabled
    Command = $broker
    Trigger = 'AppModel-Runtime/Admin 201'
    Lifetime = 'Strict AppleMusic.exe PID-bound'
    MultipleInstances = 'StopExisting'
    LoginAutostartRemoved = $true
}
