param(
 [Parameter(Mandatory = $true)][string]$PackagePath,
 [ValidateSet('x64', 'arm64')][string]$Architecture = 'x64',
 [string]$OutputDirectory = 'artifacts',
 [string]$ISCCPath = '',
 [string]$Version = '',
 [string]$PythonExe = 'python'
)
$ErrorActionPreference = 'Stop'
$taskRoot = Split-Path -Parent $PSScriptRoot
if (-not $Version) { $Version = (Get-Content -LiteralPath (Join-Path $taskRoot 'VERSION') -Raw).Trim() }
if ($Version -notmatch '^(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)(?:-[0-9A-Za-z]+(?:[.-][0-9A-Za-z]+)*)?$') { throw 'Version must be a semantic version' }
$taskComponents = @($Version.Split('-')[0].Split('.') | ForEach-Object { [int]$_ })
if (@($taskComponents | Where-Object { $_ -gt 65535 }).Count) { throw 'Windows version components must fit in 16 bits' }
if (-not $ISCCPath) { $ISCCPath = Join-Path $taskRoot 'tools\inno-6.7.3\compiler\ISCC.exe' }
$taskCompiler = (Resolve-Path -LiteralPath $ISCCPath).Path
$taskCompilerDirectory = Split-Path -Parent $taskCompiler
# The official compiler has a 0.0.0.0 file-version resource. Check the actual
# binaries extracted from the pinned 6.7.3 distribution instead.
$taskCompilerDigests = @{
 'ISCC.exe' = '0a8757031b33777e4c9cbffee40f11a5062b36d25cbe144c1db73b6102b80ad7'
 'ISCmplr.dll' = '85a1e3090d3a5b85319f001b7c8f9ecfad45f37eff030a67bbe29ef58b7aa2c3'
 'Languages\ChineseSimplified.isl' = '7d544b9bb1d142cfa11f2e5d3cc8abe2e55f8e066c5124e3772675aa236e1278'
}
foreach ($taskName in $taskCompilerDigests.Keys) {
 if ((Get-FileHash -LiteralPath (Join-Path $taskCompilerDirectory $taskName) -Algorithm SHA256).Hash.ToLowerInvariant() -ne $taskCompilerDigests[$taskName]) { throw "Expected pinned Inno Setup 6.7.3 dependency: $taskName" }
}
if ((Get-FileHash -LiteralPath $taskCompiler -Algorithm SHA256).Hash.ToLowerInvariant() -ne $taskCompilerDigests['ISCC.exe']) { throw 'ISCCPath must refer to the pinned Inno Setup 6.7.3 compiler' }
$taskPackage = (Resolve-Path -LiteralPath $PackagePath).Path
$taskOutput = if ([IO.Path]::IsPathRooted($OutputDirectory)) { [IO.Path]::GetFullPath($OutputDirectory) } else { [IO.Path]::GetFullPath((Join-Path $taskRoot $OutputDirectory)) }
$taskStageRoot = [IO.Path]::GetFullPath((Join-Path $taskRoot "build\windows-installer-$Architecture"))
$taskStage = Join-Path $taskStageRoot ([guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $taskStage -Force | Out-Null
New-Item -ItemType Directory -Path $taskOutput -Force | Out-Null
$taskPayload = Join-Path $taskStage 'payload'
$taskEntries = Join-Path $taskStage 'files.iss'
$taskInstaller = Join-Path $taskOutput "GlideBeam-$Version-Windows-$Architecture-Setup.exe"
try {
 & $PythonExe (Join-Path $taskRoot 'installer\windows\prepare_payload.py') --package $taskPackage --architecture $Architecture --version $Version --output $taskPayload --file-entries $taskEntries
 if ($LASTEXITCODE -ne 0) { throw 'Portable ZIP validation failed; installer was not compiled' }
 # Compiler binaries and matching translation were checked before staging.
 $taskArguments = @(
  '/Qp', "/O$taskOutput", "/DAppVersion=$Version", "/DNumericVersion=$($taskComponents -join '.').0",
  "/DAppArchitecture=$Architecture", "/DPayloadDirectory=$taskPayload", "/DFileEntries=$taskEntries",
  "/DAppIcon=$(Join-Path $taskRoot 'assets\app-icon.ico')", (Join-Path $taskRoot 'installer\windows\GlideBeam.iss')
 )
 & $taskCompiler @taskArguments
 if ($LASTEXITCODE -ne 0 -or -not (Test-Path -LiteralPath $taskInstaller -PathType Leaf)) { throw 'Inno Setup compilation failed' }
 $taskHash = (Get-FileHash -LiteralPath $taskInstaller -Algorithm SHA256).Hash.ToLowerInvariant()
 [IO.File]::WriteAllText("$taskInstaller.sha256", "$taskHash  $([IO.Path]::GetFileName($taskInstaller))`n", [Text.Encoding]::ASCII)
 Write-Output "Packaged $([IO.Path]::GetFileName($taskInstaller)): SHA256 $taskHash"
} finally {
 # This freshly generated staging directory is the only recursive cleanup target.
 $taskResolvedStage = [IO.Path]::GetFullPath($taskStage)
 if ($taskResolvedStage.StartsWith($taskStageRoot + [IO.Path]::DirectorySeparatorChar, [StringComparison]::OrdinalIgnoreCase) -and (Test-Path -LiteralPath $taskResolvedStage)) {
  Remove-Item -LiteralPath $taskResolvedStage -Recurse -Force
 }
}
