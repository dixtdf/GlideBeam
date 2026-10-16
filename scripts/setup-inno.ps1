param([Parameter(Mandatory)][string]$Destination, [string]$Proxy = '')
$ErrorActionPreference = 'Stop'
# Official is-6_7_3 release and its matching contributed translation.
$taskVersion = '6.7.3'
$taskInstallerDigest = '9c73c3bae7ed48d44112a0f48e66742c00090bdb5bef71d9d3c056c66e97b732'
$taskLanguageDigest = '7d544b9bb1d142cfa11f2e5d3cc8abe2e55f8e066c5124e3772675aa236e1278'
$taskDestination = [IO.Path]::GetFullPath($Destination)
if ($taskDestination.Contains('"')) { throw 'Destination cannot contain quotation marks' }
New-Item -ItemType Directory -Force -Path $taskDestination | Out-Null
function Get-PinnedFile([string]$Uri, [string]$Path, [string]$Digest) {
    if (-not (Test-Path -LiteralPath $Path)) {
        $taskRequest = @{ Uri = $Uri; OutFile = $Path; TimeoutSec = 300 }
        if ($Proxy) { $taskRequest.Proxy = $Proxy }
        Invoke-WebRequest @taskRequest
    }
    if ((Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash.ToLowerInvariant() -ne $Digest) {
        throw "Pinned dependency checksum mismatch: $Path"
    }
}
$taskInstaller = Join-Path $taskDestination "innosetup-$taskVersion.exe"
Get-PinnedFile "https://github.com/jrsoftware/issrc/releases/download/is-6_7_3/innosetup-$taskVersion.exe" $taskInstaller $taskInstallerDigest
$taskCompilerDirectory = Join-Path $taskDestination 'compiler'
$taskCompiler = Join-Path $taskCompilerDirectory 'ISCC.exe'
if (-not (Test-Path -LiteralPath $taskCompiler)) {
    # Upstream's /PORTABLE=1 disables its registry entry, shortcuts and file associations.
    $taskArguments = @('/VERYSILENT', '/SUPPRESSMSGBOXES', '/NORESTART', '/SP-', '/CURRENTUSER', '/PORTABLE=1', '/NOICONS', '/TASKS=""', "/DIR=`"$taskCompilerDirectory`"")
    $taskProcess = Start-Process -FilePath $taskInstaller -ArgumentList $taskArguments -WindowStyle Hidden -Wait -PassThru
    if ($taskProcess.ExitCode -ne 0) { throw "Inno Setup extraction failed: $($taskProcess.ExitCode)" }
}
if (-not (Test-Path -LiteralPath $taskCompiler)) { throw 'ISCC.exe is missing after portable extraction' }
# These are the compiler binaries extracted from the verified official installer.
# ISCC's file version resource is 0.0.0.0; do not use it as a version check.
$taskCompilerDigests = @{
    'ISCC.exe' = '0a8757031b33777e4c9cbffee40f11a5062b36d25cbe144c1db73b6102b80ad7'
    'ISCmplr.dll' = '85a1e3090d3a5b85319f001b7c8f9ecfad45f37eff030a67bbe29ef58b7aa2c3'
}
foreach ($taskName in $taskCompilerDigests.Keys) {
    if ((Get-FileHash -LiteralPath (Join-Path $taskCompilerDirectory $taskName) -Algorithm SHA256).Hash.ToLowerInvariant() -ne $taskCompilerDigests[$taskName]) {
        throw "Unexpected Inno $taskVersion compiler binary: $taskName"
    }
}
$taskLanguage = Join-Path $taskCompilerDirectory 'Languages/ChineseSimplified.isl'
Get-PinnedFile 'https://raw.githubusercontent.com/jrsoftware/issrc/is-6_7_3/Files/Languages/Unofficial/ChineseSimplified.isl' $taskLanguage $taskLanguageDigest
if ($env:GITHUB_ENV) { "GLIDEBEAM_ISCC=$taskCompiler" | Out-File -LiteralPath $env:GITHUB_ENV -Encoding utf8 -Append }
Write-Output $taskCompiler
