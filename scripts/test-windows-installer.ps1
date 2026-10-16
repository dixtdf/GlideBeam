param(
    [Parameter(Mandatory)][string]$InstallerPath,
    [Parameter(Mandatory)][string]$PackagePath,
    [ValidateSet('x64', 'arm64')][string]$Architecture = 'x64',
    [string]$OutputDirectory = '',
    [string]$Version = ''
)
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
$taskRoot = Split-Path -Parent $PSScriptRoot
if (-not $Version) { $Version = (Get-Content -LiteralPath (Join-Path $taskRoot 'VERSION') -Raw).Trim() }
if ($Version -notmatch '^\d+\.\d+\.\d+(?:-[0-9A-Za-z.-]+)?$') { throw 'Invalid release version.' }
$taskInstaller = (Resolve-Path -LiteralPath $InstallerPath).Path
$taskPackage = (Resolve-Path -LiteralPath $PackagePath).Path
if ([IO.Path]::GetFileName($taskInstaller) -ne "GlideBeam-$Version-Windows-$Architecture-Setup.exe") { throw 'Installer filename does not match version and architecture.' }
if ([IO.Path]::GetFileName($taskPackage) -ne "GlideBeam-$Version-Windows-$Architecture.zip") { throw 'Package filename does not match version and architecture.' }

function Assert-Checksum([string]$Path) {
    $sidecar = "$Path.sha256"
    if (-not (Test-Path -LiteralPath $sidecar -PathType Leaf)) { throw 'Required checksum sidecar is missing.' }
    $line = (Get-Content -LiteralPath $sidecar -Raw).Trim()
    if ($line -notmatch '^([0-9a-fA-F]{64}) [ *](.+)$') { throw 'Malformed checksum sidecar.' }
    $expectedHash = $Matches[1]
    if ($Matches[2] -cne [IO.Path]::GetFileName($Path)) { throw 'Checksum filename does not match.' }
    if ((Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash -ine $expectedHash) { throw 'Checksum mismatch.' }
}
Assert-Checksum $taskInstaller
Assert-Checksum $taskPackage

Add-Type -AssemblyName System.IO.Compression.FileSystem
$taskPayload = @{}
$taskArchive = [IO.Compression.ZipFile]::OpenRead($taskPackage)
try {
    $taskTotalBytes = 0L
    foreach ($entry in $taskArchive.Entries) {
        $name = $entry.FullName
        if ($name -match '(^[/\\]|\\|(^|/)\.\.?(/|$)|:|/$)' -or $taskPayload.ContainsKey($name)) { throw 'Unsafe or duplicate package path.' }
        $fileType = ($entry.ExternalAttributes -shr 16) -band 0xF000
        if ($fileType -ne 0 -and $fileType -ne 0x8000) { throw 'Package contains a non-regular file.' }
        $taskTotalBytes += $entry.Length
        if ($entry.Length -gt 64MB -or $taskTotalBytes -gt 256MB) { throw 'Unexpected package size.' }
        $stream = $entry.Open()
        $hasher = [Security.Cryptography.SHA256]::Create()
        try { $digest = [BitConverter]::ToString($hasher.ComputeHash($stream)).Replace('-', '') }
        finally { $hasher.Dispose(); $stream.Dispose() }
        $taskPayload.Add($name, $digest)
        if ($name -match '\.(exe|dll)$') {
            $stream = $entry.Open()
            $memory = New-Object IO.MemoryStream
            try {
                $stream.CopyTo($memory)
                $bytes = $memory.ToArray()
                if ($bytes.Length -lt 64 -or [BitConverter]::ToUInt16($bytes, 0) -ne 0x5A4D) { throw 'Package runtime has no DOS header.' }
                $offset = [BitConverter]::ToInt32($bytes, 0x3C)
                if ($offset -lt 64 -or $offset -gt $bytes.Length - 26 -or [BitConverter]::ToUInt32($bytes, $offset) -ne 0x4550) { throw 'Package runtime has no PE header.' }
                $expectedMachine = if ($Architecture -eq 'x64') { 0x8664 } else { 0xAA64 }
                if ([BitConverter]::ToUInt16($bytes, $offset + 4) -ne $expectedMachine -or [BitConverter]::ToUInt16($bytes, $offset + 24) -ne 0x20B) { throw 'Package runtime architecture mismatch.' }
            } finally { $memory.Dispose(); $stream.Dispose() }
        }
    }
} finally { $taskArchive.Dispose() }
foreach ($required in @('GlideBeam.exe', 'remote_transport.dll', 'Start-GlideBeam.cmd', 'THIRD-PARTY-LICENSES.txt')) {
    if (-not $taskPayload.ContainsKey($required)) { throw "Missing installer payload file: $required" }
}
$taskRuntimeLibrary = if ($Architecture -eq 'x64') { 'libwinpthread-1.dll' } else { 'libunwind.dll' }
if (-not $taskPayload.ContainsKey($taskRuntimeLibrary)) { throw 'Missing architecture runtime library.' }
$taskSetupVersion = [Diagnostics.FileVersionInfo]::GetVersionInfo($taskInstaller)
$taskNumericVersion = ($Version -split '-')[0]
if ($taskSetupVersion.FileVersion.Trim() -notmatch ('^' + [regex]::Escape($taskNumericVersion) + '(?:\.0)?$') -or $taskSetupVersion.ProductVersion.Trim() -cne $Version) { throw 'Installer embedded version mismatch.' }
if ($taskSetupVersion.FileDescription.Trim() -cne "GlideBeam Setup ($Architecture)") { throw 'Installer embedded architecture description mismatch.' }
Write-Output "PASS $Architecture package architecture, checksums and Setup version"
if ($Architecture -eq 'arm64') {
    Write-Output 'ARM64 static verification only; installer and application were not executed.'
    return
}
if (-not [Environment]::Is64BitOperatingSystem) { throw 'The installer lifecycle requires 64-bit Windows.' }

$taskAppId = '{6AFB2AC5-3180-45B7-BD57-1485C19F47C3}_is1'
$taskUninstallKey = "Software\Microsoft\Windows\CurrentVersion\Uninstall\$taskAppId"
function Get-InstallRegistration([Microsoft.Win32.RegistryHive]$Hive, [Microsoft.Win32.RegistryView]$View) {
    $base = [Microsoft.Win32.RegistryKey]::OpenBaseKey($Hive, $View)
    try {
        $key = $base.OpenSubKey($taskUninstallKey)
        if (-not $key) { return $null }
        try {
            return @{
                DisplayName = $key.GetValue('DisplayName'); DisplayVersion = $key.GetValue('DisplayVersion')
                Publisher = $key.GetValue('Publisher'); InstallLocation = $key.GetValue('InstallLocation')
                UninstallString = $key.GetValue('UninstallString'); QuietUninstallString = $key.GetValue('QuietUninstallString')
            }
        } finally { $key.Dispose() }
    } finally { $base.Dispose() }
}
foreach ($hive in @([Microsoft.Win32.RegistryHive]::CurrentUser, [Microsoft.Win32.RegistryHive]::LocalMachine)) {
    foreach ($view in @([Microsoft.Win32.RegistryView]::Registry32, [Microsoft.Win32.RegistryView]::Registry64)) {
        if ($null -ne (Get-InstallRegistration $hive $view)) { throw 'An existing GlideBeam installation is registered. Refusing to modify it; use a clean CI account.' }
    }
}
$taskDesktopShortcut = Join-Path ([Environment]::GetFolderPath('DesktopDirectory', [Environment+SpecialFolderOption]::DoNotVerify)) 'GlideBeam.lnk'
if (Test-Path -LiteralPath $taskDesktopShortcut) { throw 'An existing GlideBeam desktop shortcut exists. Refusing to modify it.' }

if (-not $OutputDirectory) {
    $taskOutputBase = if ($env:RUNNER_TEMP) { Join-Path $env:RUNNER_TEMP 'glidebeam-installer-tests' } else { Join-Path $taskRoot 'build\installer-tests' }
} else { $taskOutputBase = [IO.Path]::GetFullPath($OutputDirectory) }
$taskTestRoot = [IO.Path]::GetFullPath((Join-Path $taskOutputBase ([guid]::NewGuid().ToString('N'))))
$taskInstallDirectory = [IO.Path]::GetFullPath((Join-Path $taskTestRoot 'installed app'))
if (-not $taskInstallDirectory.StartsWith($taskTestRoot.TrimEnd('\') + '\', [StringComparison]::OrdinalIgnoreCase)) { throw 'Test install directory escaped the isolated fixture.' }
New-Item -ItemType Directory -Path $taskTestRoot -Force | Out-Null
$taskGroupName = 'GlideBeam'
$taskGroupDirectory = Join-Path ([Environment]::GetFolderPath('Programs', [Environment+SpecialFolderOption]::DoNotVerify)) $taskGroupName
if (Test-Path -LiteralPath $taskGroupDirectory) { throw 'An existing GlideBeam Start Menu group exists. Refusing to modify it.' }
$taskAppShortcut = Join-Path $taskGroupDirectory 'GlideBeam.lnk'
$taskUninstallShortcut = Join-Path $taskGroupDirectory 'Uninstall GlideBeam.lnk'
$taskUninstaller = Join-Path $taskInstallDirectory 'unins000.exe'
$taskSentinel = Join-Path $taskInstallDirectory 'user-created-preserve.txt'
$taskSentinelContents = 'Installer lifecycle sentinel ' + [guid]::NewGuid().ToString('N')
if (-not $env:LOCALAPPDATA) { throw 'LOCALAPPDATA is required for the configuration preservation fixture.' }
$taskConfigurationDirectory = Join-Path $env:LOCALAPPDATA 'RemoteNative'
if ((Test-Path -LiteralPath $taskConfigurationDirectory) -and ((Get-Item -LiteralPath $taskConfigurationDirectory).Attributes -band [IO.FileAttributes]::ReparsePoint)) { throw 'Refusing to write a test fixture through a redirected configuration directory.' }
$taskConfigurationMarker = Join-Path $taskConfigurationDirectory ('installer-preservation-test-' + [guid]::NewGuid().ToString('N') + '.txt')
$taskConfigurationMarkerContents = 'Inert installer preservation fixture ' + [guid]::NewGuid().ToString('N')
$taskConfigurationDirectoryCreated = $false
$taskConfigurationMarkerCreated = $false

# Read hashes only. Never modify live pairing, preferences or diagnostic logs.
function Get-ConfigurationSnapshot {
    $snapshot = @{}
    foreach ($basePath in @($env:LOCALAPPDATA, $env:APPDATA)) {
        if (-not $basePath) { continue }
        foreach ($leaf in @('connection.json', 'ui-language.txt')) {
            $path = Join-Path (Join-Path $basePath 'RemoteNative') $leaf
            $snapshot[$path] = if (Test-Path -LiteralPath $path -PathType Leaf) { (Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash } else { $null }
        }
    }
    return $snapshot
}
$taskConfigurationBefore = Get-ConfigurationSnapshot
function Assert-ConfigurationPreserved {
    $after = Get-ConfigurationSnapshot
    foreach ($path in $taskConfigurationBefore.Keys) {
        if ($taskConfigurationBefore[$path] -cne $after[$path]) { throw 'Existing user configuration changed during installer lifecycle.' }
    }
    if (-not (Test-Path -LiteralPath $taskConfigurationMarker -PathType Leaf) -or [IO.File]::ReadAllText($taskConfigurationMarker) -cne $taskConfigurationMarkerContents) { throw 'Installer modified or removed the independent configuration preservation fixture.' }
}
$taskExistingProcesses = @(Get-Process -Name GlideBeam, RemoteNative -ErrorAction SilentlyContinue | Select-Object -ExpandProperty Id)
function Assert-NoApplicationLaunch {
    $newProcesses = @(Get-Process -Name GlideBeam, RemoteNative -ErrorAction SilentlyContinue | Where-Object { $_.Id -notin $taskExistingProcesses })
    if ($newProcesses.Count -gt 0) { throw 'Silent installer unexpectedly launched the application.' }
}
function Invoke-InstallerProcess([string]$Executable, [string[]]$Arguments) {
    $process = Start-Process -FilePath $Executable -ArgumentList $Arguments -PassThru -WindowStyle Hidden
    if (-not $process.WaitForExit(120000)) {
        $process.Kill()
        throw 'Installer process exceeded the lifecycle test timeout.'
    }
    $process.Refresh()
    if ($process.ExitCode -ne 0) { throw "Installer process failed with exit code $($process.ExitCode). See isolated installer logs." }
}
function Assert-InstalledPayload {
    foreach ($name in $taskPayload.Keys) {
        $path = Join-Path $taskInstallDirectory $name
        if (-not (Test-Path -LiteralPath $path -PathType Leaf)) { throw "Installed payload file is missing: $name" }
        if ((Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash -cne $taskPayload[$name]) { throw "Installed payload differs from the portable package: $name" }
    }
    $metadata = [Diagnostics.FileVersionInfo]::GetVersionInfo((Join-Path $taskInstallDirectory 'GlideBeam.exe'))
    if ($metadata.FileVersion.Trim() -notmatch ('^' + [regex]::Escape($taskNumericVersion) + '(?:\.0)?$')) { throw 'Installed application version mismatch.' }
}
function Assert-Registration {
    $registration = Get-InstallRegistration ([Microsoft.Win32.RegistryHive]::CurrentUser) ([Microsoft.Win32.RegistryView]::Registry64)
    if (-not $registration -or $registration.DisplayName -ne 'GlideBeam' -or $registration.DisplayVersion -cne $Version -or $registration.Publisher -ne 'dixtdf') { throw 'Per-user Installed Apps registration is missing or incorrect.' }
    if ([IO.Path]::GetFullPath($registration.InstallLocation).TrimEnd('\') -ine $taskInstallDirectory) { throw 'Installed Apps registration points outside the isolated install directory.' }
    if ($registration.UninstallString -notlike "*$taskUninstaller*" -or $registration.QuietUninstallString -notlike "*$taskUninstaller*") { throw 'Uninstall registration points to an unexpected executable.' }
    if ($null -ne (Get-InstallRegistration ([Microsoft.Win32.RegistryHive]::LocalMachine) ([Microsoft.Win32.RegistryView]::Registry64))) { throw 'Per-user installer unexpectedly registered a machine-wide application.' }
}
$taskShell = New-Object -ComObject WScript.Shell
function Assert-Shortcut([string]$Path, [string]$Target) {
    if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) { throw 'Installer shortcut is missing.' }
    $shortcut = $taskShell.CreateShortcut($Path)
    try {
        if ([IO.Path]::GetFullPath($shortcut.TargetPath) -ine $Target) { throw 'Installer shortcut points to an unexpected target.' }
    } finally { [void][Runtime.InteropServices.Marshal]::FinalReleaseComObject($shortcut) }
}
function Invoke-TestInstall([string]$LogName) {
    Invoke-InstallerProcess $taskInstaller @('/VERYSILENT', '/SUPPRESSMSGBOXES', '/NORESTART', '/SP-', '/NOCLOSEAPPLICATIONS', '/NORESTARTAPPLICATIONS', '/TASKS=desktopicon', ('/DIR="' + $taskInstallDirectory + '"'), ('/LOG="' + (Join-Path $taskTestRoot $LogName) + '"'))
}
$taskCleanupNeeded = $false
try {
    # This inert, uniquely named marker makes retention meaningful on clean CI.
    # Existing connection.json and ui-language.txt are never written by the test.
    if (-not (Test-Path -LiteralPath $taskConfigurationDirectory)) {
        New-Item -ItemType Directory -Path $taskConfigurationDirectory | Out-Null
        $taskConfigurationDirectoryCreated = $true
    }
    [IO.File]::WriteAllText($taskConfigurationMarker, $taskConfigurationMarkerContents)
    $taskConfigurationMarkerCreated = $true
    $taskCleanupNeeded = $true
    Invoke-TestInstall 'install.log'
    Assert-InstalledPayload
    Assert-Registration
    Assert-Shortcut $taskAppShortcut (Join-Path $taskInstallDirectory 'GlideBeam.exe')
    Assert-Shortcut $taskUninstallShortcut $taskUninstaller
    Assert-Shortcut $taskDesktopShortcut (Join-Path $taskInstallDirectory 'GlideBeam.exe')
    Assert-ConfigurationPreserved
    Assert-NoApplicationLaunch
    Write-Output 'PASS silent per-user install, payload hashes, Installed Apps registration and shortcuts'

    [IO.File]::WriteAllText($taskSentinel, $taskSentinelContents)
    [IO.File]::AppendAllText((Join-Path $taskInstallDirectory 'remote_transport.dll'), 'installer repair fixture')
    Invoke-TestInstall 'repair.log'
    Assert-InstalledPayload
    Assert-Registration
    Assert-Shortcut $taskAppShortcut (Join-Path $taskInstallDirectory 'GlideBeam.exe')
    if ([IO.File]::ReadAllText($taskSentinel) -cne $taskSentinelContents) { throw 'Reinstall modified an unknown user file.' }
    Assert-ConfigurationPreserved
    Assert-NoApplicationLaunch
    Write-Output 'PASS same-version upgrade/repair restores owned files and preserves unknown files and configuration'

    Invoke-InstallerProcess $taskUninstaller @('/VERYSILENT', '/SUPPRESSMSGBOXES', '/NORESTART', ('/LOG="' + (Join-Path $taskTestRoot 'uninstall.log') + '"'))
    $taskCleanupNeeded = $false
    foreach ($name in $taskPayload.Keys) {
        if (Test-Path -LiteralPath (Join-Path $taskInstallDirectory $name)) { throw "Uninstall retained an owned payload file: $name" }
    }
    foreach ($path in @($taskAppShortcut, $taskUninstallShortcut, $taskDesktopShortcut)) {
        if (Test-Path -LiteralPath $path) { throw 'Uninstall retained an application shortcut.' }
    }
    foreach ($hive in @([Microsoft.Win32.RegistryHive]::CurrentUser, [Microsoft.Win32.RegistryHive]::LocalMachine)) {
        foreach ($view in @([Microsoft.Win32.RegistryView]::Registry32, [Microsoft.Win32.RegistryView]::Registry64)) {
            if ($null -ne (Get-InstallRegistration $hive $view)) { throw 'Uninstall retained application registration.' }
        }
    }
    if (-not (Test-Path -LiteralPath $taskSentinel -PathType Leaf) -or [IO.File]::ReadAllText($taskSentinel) -cne $taskSentinelContents) { throw 'Uninstall removed an unknown user file.' }
    Assert-ConfigurationPreserved
    Assert-NoApplicationLaunch
    Write-Output 'PASS uninstall removes owned files, registration and shortcuts while preserving user files and configuration'
    Write-Output "Installer lifecycle logs: $taskTestRoot"
} finally {
    try {
        if ($taskCleanupNeeded -and (Test-Path -LiteralPath $taskUninstaller -PathType Leaf)) {
            # Only invoke the uninstaller inside this freshly generated test directory.
            if (-not ([IO.Path]::GetFullPath($taskUninstaller)).StartsWith($taskTestRoot.TrimEnd('\') + '\', [StringComparison]::OrdinalIgnoreCase)) { throw 'Refusing cleanup outside the isolated test directory.' }
            Invoke-InstallerProcess $taskUninstaller @('/VERYSILENT', '/SUPPRESSMSGBOXES', '/NORESTART', ('/LOG="' + (Join-Path $taskTestRoot 'cleanup.log') + '"'))
        }
    } finally {
        if ($taskConfigurationMarkerCreated -and (Test-Path -LiteralPath $taskConfigurationMarker -PathType Leaf)) {
            if ([IO.File]::ReadAllText($taskConfigurationMarker) -ceq $taskConfigurationMarkerContents) {
                Remove-Item -LiteralPath $taskConfigurationMarker
            } else { Write-Warning 'Configuration test marker was modified and has been retained for inspection.' }
        }
        if ($taskConfigurationDirectoryCreated -and (Test-Path -LiteralPath $taskConfigurationDirectory) -and [IO.Directory]::GetFileSystemEntries($taskConfigurationDirectory).Length -eq 0) {
            [IO.Directory]::Delete($taskConfigurationDirectory, $false)
        }
        [void][Runtime.InteropServices.Marshal]::FinalReleaseComObject($taskShell)
    }
}
