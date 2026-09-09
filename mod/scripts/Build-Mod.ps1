[CmdletBinding()]
param(
    [ValidateSet('Debug', 'Release')][string]$Configuration = 'Release',
    [string]$BuildDirectory = 'build'
)

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$build = if ([IO.Path]::IsPathRooted($BuildDirectory)) {
    $BuildDirectory
} else {
    Join-Path $root $BuildDirectory
}

$cmakePath = (Get-Command cmake.exe -ErrorAction SilentlyContinue).Source
$clPath = (Get-Command cl.exe -ErrorAction SilentlyContinue).Source

if (-not $clPath) {
    $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
    if (Test-Path -LiteralPath $vswhere) {
        $vsRoot = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
        if ($vsRoot) {
            $devShell = Join-Path $vsRoot 'Common7\Tools\Launch-VsDevShell.ps1'
            if (Test-Path -LiteralPath $devShell) {
                & $devShell -Arch amd64 -HostArch amd64 -SkipAutomaticLocation
                $clPath = (Get-Command cl.exe -ErrorAction SilentlyContinue).Source
            }
            if (-not $cmakePath) {
                $bundledCmake = Join-Path $vsRoot 'Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe'
                if (Test-Path -LiteralPath $bundledCmake) { $cmakePath = $bundledCmake }
            }
        }
    }
}

if (-not $cmakePath) { throw 'cmake.exe was not found in PATH or the selected Visual Studio Build Tools instance.' }
if (-not $clPath) { throw 'An x64 MSVC compiler could not be activated through Visual Studio DevShell.' }

$package = Get-AppxPackage -Name AppleInc.AppleMusicWin -ErrorAction Stop

& $cmakePath -S $root -B $build -A x64 "-DAPPLE_MUSIC_PACKAGE_DIR=$($package.InstallLocation)"
if ($LASTEXITCODE -ne 0) { throw "CMake configure failed: $LASTEXITCODE" }

& $cmakePath --build $build --config $Configuration --parallel
if ($LASTEXITCODE -ne 0) { throw "CMake build failed: $LASTEXITCODE" }

$output = Join-Path $build $Configuration
$setupPath = Join-Path $output 'AMExclusive-Setup.exe'
if (-not (Test-Path -LiteralPath $setupPath -PathType Leaf)) {
    throw "Self-contained setup executable was not produced: $setupPath"
}

$distributionDirectory = Join-Path $build 'dist'
if (Test-Path -LiteralPath $distributionDirectory) {
    Remove-Item -LiteralPath $distributionDirectory -Recurse -Force
}
New-Item -ItemType Directory -Path $distributionDirectory -Force | Out-Null
$distributionSetup = Join-Path $distributionDirectory 'AMExclusive-Setup.exe'
Copy-Item -LiteralPath $setupPath -Destination $distributionSetup -Force

$productionFiles = @(
    'am-exclusive-broker.exe',
    'am_exclusive_media_control.exe',
    'am-exclusive-hook.dll',
    'am-exclusive-ui.dll',
    'am-exclusive.ini',
    'AMExclusive-Setup.exe'
)
Get-ChildItem -LiteralPath $output |
    Where-Object { $_.Name -in $productionFiles } |
    Select-Object FullName, Length, LastWriteTime

Write-Output "Single-file distribution: $distributionSetup"
