param([Parameter(Mandatory)][string]$Destination, [string]$Proxy = '')
$ErrorActionPreference = 'Stop'
# Version and digest are pinned to the maintainer's official release asset.
$taskToolchainVersion = '20261006'
$taskArchiveName = "llvm-mingw-$taskToolchainVersion-ucrt-x86_64.zip"
$taskDigest = '317492c456aa27ee607a5919f1d2d38dcdc1112516a24d0bf4b00d078f52d17a'
$taskDestination = [IO.Path]::GetFullPath($Destination)
New-Item -ItemType Directory -Force -Path $taskDestination | Out-Null
$taskArchive = Join-Path $taskDestination $taskArchiveName
if (-not (Test-Path -LiteralPath $taskArchive)) {
 $taskRequest = @{ Uri = "https://github.com/mstorsjo/llvm-mingw/releases/download/$taskToolchainVersion/$taskArchiveName"; OutFile = $taskArchive; TimeoutSec = 300 }
 if ($Proxy) { $taskRequest.Proxy = $Proxy }
 Invoke-WebRequest @taskRequest
}
if ((Get-FileHash -LiteralPath $taskArchive -Algorithm SHA256).Hash.ToLowerInvariant() -ne $taskDigest) { throw 'LLVM-MinGW archive checksum does not match the pinned official release' }
$taskBinaryDirectory = Join-Path $taskDestination "llvm-mingw-$taskToolchainVersion-ucrt-x86_64\bin"
if (-not (Test-Path -LiteralPath (Join-Path $taskBinaryDirectory 'aarch64-w64-mingw32-clang++.exe'))) {
 Expand-Archive -LiteralPath $taskArchive -DestinationPath $taskDestination -Force
}
if (-not (Test-Path -LiteralPath (Join-Path $taskBinaryDirectory 'aarch64-w64-mingw32-clang++.exe'))) { throw 'LLVM-MinGW cross compiler was not found after extraction' }
if ($env:GITHUB_PATH) { $taskBinaryDirectory | Out-File -LiteralPath $env:GITHUB_PATH -Encoding utf8 -Append }
if ($env:GITHUB_ENV) { "GLIDEBEAM_TOOLCHAIN_BIN=$taskBinaryDirectory" | Out-File -LiteralPath $env:GITHUB_ENV -Encoding utf8 -Append }
Write-Output $taskBinaryDirectory
