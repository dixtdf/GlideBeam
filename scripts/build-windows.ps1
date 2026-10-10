param(
 [switch]$SkipRust,
 [string]$OutputDirectory = 'dist\windows',
 [string]$ToolchainDirectory = '',
 [string]$CargoExe = '',
 [ValidateSet('x64', 'arm64')][string]$Architecture = 'x64',
 [string]$RustTarget = '',
 [string]$Version = ''
)
$ErrorActionPreference = 'Stop'
$taskRoot = Split-Path -Parent $PSScriptRoot
$taskOriginalPath = $env:PATH
$taskOriginalCargoHome = $env:CARGO_HOME
$taskOriginalRustupHome = $env:RUSTUP_HOME
$taskOriginalArmLinker = $env:CARGO_TARGET_AARCH64_PC_WINDOWS_GNULLVM_LINKER
$taskOriginalArmCompiler = $env:CC_aarch64_pc_windows_gnullvm
$taskOriginalHostLinker = $env:CARGO_TARGET_X86_64_PC_WINDOWS_GNU_LINKER
Push-Location $taskRoot
try {
 if (-not $Version) { $Version = (Get-Content -LiteralPath (Join-Path $taskRoot 'VERSION') -Raw).Trim() }
 if ($Version -notmatch '^(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)(?:-[0-9A-Za-z]+(?:[.-][0-9A-Za-z]+)*)?$') { throw 'Version must be a semantic version such as 0.2.0' }
 $taskComponents = @($Version.Split('-')[0].Split('.') | ForEach-Object { [int]$_ })
 if (@($taskComponents | Where-Object { $_ -gt 65535 }).Count) { throw 'Windows version components must fit in 16 bits' }
 if (-not $ToolchainDirectory) {
  $taskBundledGcc = Join-Path $taskRoot 'tools\mingw64\bin'
  if ($Architecture -eq 'x64' -and (Test-Path -LiteralPath (Join-Path $taskBundledGcc 'g++.exe')) -and $env:CI -ne 'true') { $ToolchainDirectory = $taskBundledGcc }
  else {
   $taskCompilerCommand = if ($Architecture -eq 'arm64') { 'aarch64-w64-mingw32-clang++.exe' } else { 'g++.exe' }
   $ToolchainDirectory = Split-Path -Parent (Get-Command $taskCompilerCommand -ErrorAction Stop).Source
  }
 }
 $taskCompilerBin = [IO.Path]::GetFullPath($ToolchainDirectory)
 $env:PATH = "$taskCompilerBin;$env:PATH"
 if (-not $CargoExe) {
  $taskBundledCargo = Join-Path $taskRoot 'tools\cargo\bin\cargo.exe'
  if ((Test-Path -LiteralPath $taskBundledCargo) -and $env:CI -ne 'true') {
   $env:CARGO_HOME = Join-Path $taskRoot 'tools\cargo'
   $env:RUSTUP_HOME = Join-Path $taskRoot 'tools\rustup'
   $CargoExe = $taskBundledCargo
   if ($Architecture -eq 'arm64') {
    $taskHostGcc = Join-Path $taskRoot 'tools\mingw64\bin\gcc.exe'
    if (Test-Path -LiteralPath $taskHostGcc) { $env:CARGO_TARGET_X86_64_PC_WINDOWS_GNU_LINKER = $taskHostGcc }
   }
  } else { $CargoExe = (Get-Command cargo.exe -ErrorAction Stop).Source }
 }
 if ($Architecture -eq 'arm64') {
  if (-not $RustTarget) { $RustTarget = 'aarch64-pc-windows-gnullvm' }
  if ($RustTarget -ne 'aarch64-pc-windows-gnullvm') { throw 'ARM64 requires the aarch64-pc-windows-gnullvm Rust target' }
  $env:CARGO_TARGET_AARCH64_PC_WINDOWS_GNULLVM_LINKER = Join-Path $taskCompilerBin 'aarch64-w64-mingw32-clang.exe'
  $env:CC_aarch64_pc_windows_gnullvm = $env:CARGO_TARGET_AARCH64_PC_WINDOWS_GNULLVM_LINKER
 } elseif ($RustTarget -and $RustTarget -ne 'x86_64-pc-windows-gnu') { throw 'x64 requires the x86_64-pc-windows-gnu Rust target' }
 $taskCargoArguments = @('build', '--locked', '--release', '-p', 'remote_transport')
 $taskLibrary = Join-Path $taskRoot 'target\release\remote_transport.dll'
 if ($RustTarget) {
  $taskCargoArguments += @('--target', $RustTarget)
  $taskLibrary = Join-Path $taskRoot "target\$RustTarget\release\remote_transport.dll"
 }
 if (-not $SkipRust) {
  & $CargoExe @taskCargoArguments
  if ($LASTEXITCODE -ne 0) { throw 'Rust transport build failed' }
 }
 $taskOutput = if ([IO.Path]::IsPathRooted($OutputDirectory)) { [IO.Path]::GetFullPath($OutputDirectory) } else { [IO.Path]::GetFullPath((Join-Path $taskRoot $OutputDirectory)) }
 New-Item -ItemType Directory -Force -Path $taskOutput | Out-Null
 $taskResourceDirectory = Join-Path $taskRoot "build\windows-$Architecture"
 New-Item -ItemType Directory -Force -Path $taskResourceDirectory | Out-Null
 # Generate versioned resources without modifying the checkout's source files.
 $taskResourceText = Get-Content -LiteralPath 'apps\windows-client\resources.rc' -Raw
 $taskResourceText = $taskResourceText -replace '(?m)^\s*FILEVERSION\s+[^\r\n]+', (' FILEVERSION ' + ($taskComponents -join ',') + ',0')
 $taskResourceText = $taskResourceText -replace '(?m)^\s*PRODUCTVERSION\s+[^\r\n]+', (' PRODUCTVERSION ' + ($taskComponents -join ',') + ',0')
 $taskResourceText = $taskResourceText -replace '(VALUE "(?:FileVersion|ProductVersion)", ")[^"\\]+', ('${1}' + $Version)
 $taskAsset = (Join-Path $taskRoot 'assets\app-icon.ico').Replace('\', '/')
 $taskManifest = (Join-Path $taskRoot 'apps\windows-client\RemoteNative.manifest').Replace('\', '/')
 $taskResourceText = $taskResourceText -replace '"../../assets/app-icon.ico"', ('"' + $taskAsset + '"')
 $taskResourceText = $taskResourceText -replace '"RemoteNative.manifest"', ('"' + $taskManifest + '"')
 $taskGeneratedResource = Join-Path $taskResourceDirectory 'versioned-resources.rc'
 [IO.File]::WriteAllText($taskGeneratedResource, $taskResourceText, [Text.UTF8Encoding]::new($false))
 $taskResourceObject = Join-Path $taskResourceDirectory 'resources.o'
 $taskResourceCompiler = if ($Architecture -eq 'arm64') { 'aarch64-w64-mingw32-windres.exe' } else { 'windres.exe' }
 & (Join-Path $taskCompilerBin $taskResourceCompiler) --codepage=65001 --include-dir apps/windows-client --input $taskGeneratedResource --output $taskResourceObject --output-format coff
 if ($LASTEXITCODE -ne 0) { throw 'Windows icon/version resource build failed' }
 $taskSources = @('apps/windows-client/main.cpp', 'apps/windows-client/smoke.cpp', 'apps/windows-client/decode-smoke.cpp', 'apps/windows-client/ui.cpp', 'apps/windows-client/ui-watchdog.cpp', 'apps/windows-client/session.cpp', 'apps/windows-client/exchange-windows.cpp', 'shared/exchange.cpp', 'shared/diagnostic-log.cpp', 'apps/windows-client/video.cpp')
 if (Test-Path -LiteralPath 'shared/i18n.cpp') { $taskSources += 'shared/i18n.cpp' }
 $taskCompilerOptions = @('-std=c++20', '-O2', '-Wall', '-Wextra', '-Werror', '-DUNICODE', '-D_UNICODE', '-D_WIN32_WINNT=0x0A00', '-DWINVER=0x0A00', '-municode', '-mwindows')
 if ($Architecture -eq 'arm64') {
  $taskCompiler = Join-Path $taskCompilerBin 'aarch64-w64-mingw32-clang++.exe'
  # The pinned LLVM-MinGW Clang does not inject GCC's default-manifest.o.
  # Link our resources directly and statically link its C++ runtime.
  $taskCompilerOptions += '-static'
  $taskLinkResources = @($taskResourceObject)
 } else {
  $taskCompiler = Join-Path $taskCompilerBin 'g++.exe'
  $taskCompilerOptions += @('-specs=apps/windows-client/native-manifest.specs', '-static-libgcc', '-static-libstdc++')
  $taskLinkResources = @($taskResourceObject)
 }
 & $taskCompiler @taskCompilerOptions @taskSources @taskLinkResources -o (Join-Path $taskOutput 'RemoteNative.exe') -ld2d1 -ldwrite -lwindowscodecs -ldwmapi -limm32 -lshell32 -lole32 -loleaut32 -luuid -lstrmiids -lcrypt32 -ld3d11 -ldxgi -lmfplat -lmf -lmfuuid -lwmcodecdspuuid -lcomctl32
 if ($LASTEXITCODE -ne 0) { throw 'Windows client build failed' }
 Copy-Item -LiteralPath $taskLibrary -Destination (Join-Path $taskOutput 'remote_transport.dll') -Force
 if ($Architecture -eq 'x64') {
  Copy-Item -LiteralPath (Join-Path $taskCompilerBin 'libwinpthread-1.dll') -Destination (Join-Path $taskOutput 'libwinpthread-1.dll') -Force
 } else {
  $taskArmRuntime = Join-Path (Split-Path -Parent $taskCompilerBin) 'aarch64-w64-mingw32\bin\libunwind.dll'
  Copy-Item -LiteralPath $taskArmRuntime -Destination (Join-Path $taskOutput 'libunwind.dll') -Force
 }
 Write-Output "Built $taskOutput\RemoteNative.exe ($Version, $Architecture)"
} finally {
 $env:PATH = $taskOriginalPath
 $env:CARGO_HOME = $taskOriginalCargoHome
 $env:RUSTUP_HOME = $taskOriginalRustupHome
 $env:CARGO_TARGET_AARCH64_PC_WINDOWS_GNULLVM_LINKER = $taskOriginalArmLinker
 $env:CC_aarch64_pc_windows_gnullvm = $taskOriginalArmCompiler
 $env:CARGO_TARGET_X86_64_PC_WINDOWS_GNU_LINKER = $taskOriginalHostLinker
 Pop-Location
}
