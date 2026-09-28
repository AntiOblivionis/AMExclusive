[CmdletBinding()]
param(
    [Parameter(Mandatory)][string]$OutputDirectory
)

$ErrorActionPreference = 'Stop'
$expectedHash = 'A824AB1E380E384C382CEC14AC5DAD9D4C1B91F519D512082F0E7E5D6AC109DE'
# Extract only Xaml's metadata closure; no DLLs are installed or shipped.
$packages = @(
    @{ Id = 'microsoft.windowsappsdk.winui'; Version = '1.8.251222000'; Entries = @(
        'metadata/Microsoft.UI.Xaml.winmd', 'metadata/Microsoft.UI.Text.winmd') },
    @{ Id = 'microsoft.windowsappsdk.interactiveexperiences'; Version = '1.8.251217001'; Entries = @(
        'metadata/10.0.17763.0/Microsoft.UI.winmd',
        'metadata/10.0.17763.0/Microsoft.Foundation.winmd',
        'metadata/10.0.17763.0/Microsoft.Graphics.winmd') },
    @{ Id = 'microsoft.windowsappsdk.foundation'; Version = '1.8.251220000'; Entries = @(
        'metadata/Microsoft.Windows.ApplicationModel.Resources.winmd') },
    @{ Id = 'microsoft.web.webview2'; Version = '1.0.3179.45'; Entries = @(
        'lib/Microsoft.Web.WebView2.Core.winmd') }
)
$output = New-Item -ItemType Directory -Path $OutputDirectory -Force
Add-Type -AssemblyName System.IO.Compression.FileSystem
foreach ($package in $packages) {
    $uri = "https://api.nuget.org/v3-flatcontainer/$($package.Id)/$($package.Version)/$($package.Id).$($package.Version).nupkg"
    $archive = Join-Path ([IO.Path]::GetTempPath()) ("ame-winui-" + [Guid]::NewGuid().ToString('N') + '.nupkg')
    try {
        Write-Output "Downloading $($package.Id) $($package.Version)"
        Invoke-WebRequest -Uri $uri -OutFile $archive -TimeoutSec 300
        $zip = [IO.Compression.ZipFile]::OpenRead($archive)
        try {
            foreach ($entryPath in $package.Entries) {
                $entry = $zip.GetEntry($entryPath)
                if (-not $entry) { throw "Missing metadata in $($package.Id): $entryPath" }
                $destination = Join-Path $output.FullName ([IO.Path]::GetFileName($entryPath))
                [IO.Compression.ZipFileExtensions]::ExtractToFile($entry, $destination, $true)
            }
        } finally {
            $zip.Dispose()
        }
    } finally {
        if (Test-Path -LiteralPath $archive) { Remove-Item -LiteralPath $archive -Force }
    }
}
$destination = Join-Path $output.FullName 'Microsoft.UI.Xaml.winmd'
$stream = [IO.File]::OpenRead($destination)
try {
    $sha256 = [Security.Cryptography.SHA256]::Create()
    try {
        $actualHash = [BitConverter]::ToString($sha256.ComputeHash($stream)).Replace('-', '')
    } finally {
        $sha256.Dispose()
    }
} finally {
    $stream.Dispose()
}
if ($actualHash -ne $expectedHash) {
    Remove-Item -LiteralPath $destination -Force
    throw "WinUI metadata SHA-256 mismatch: expected $expectedHash; received $actualHash"
}
Write-Output "Verified Microsoft.UI.Xaml.winmd: $actualHash"
