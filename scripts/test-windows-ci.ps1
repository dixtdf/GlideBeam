param([Parameter(Mandatory)][string]$RuntimeDirectory, [string]$ToolchainDirectory = '', [string]$OutputDirectory = 'build\ci-tests\windows')
$ErrorActionPreference = 'Stop'
$taskRoot = Split-Path -Parent $PSScriptRoot
Push-Location $taskRoot
try {
 $taskRuntime = [IO.Path]::GetFullPath($RuntimeDirectory)
 $taskOutput = [IO.Path]::GetFullPath($OutputDirectory)
 New-Item -ItemType Directory -Force -Path $taskOutput | Out-Null
 if (-not $ToolchainDirectory) { $ToolchainDirectory = Split-Path -Parent (Get-Command g++.exe -ErrorAction Stop).Source }
 $taskCompiler = Join-Path $ToolchainDirectory 'g++.exe'
 $taskOriginalPath = $env:PATH
 $env:PATH = "$ToolchainDirectory;$env:PATH"
 function Invoke-Checked([string]$Executable, [string[]]$Arguments) {
  & $Executable @Arguments
  if ($LASTEXITCODE -ne 0) { throw "Test or compilation failed: $Executable" }
 }
 function Invoke-ClientTest([string]$Flag, [string]$ReportName) {
  # These modes exclusively use isolated synthetic clipboard and file fixtures.
  $taskArguments = @($Flag)
  if ($ReportName) { $taskArguments += ('"' + (Join-Path $taskOutput $ReportName) + '"') }
  $taskProcess = Start-Process -FilePath (Join-Path $taskRuntime 'RemoteNative.exe') -ArgumentList $taskArguments -PassThru -WindowStyle Hidden
  if (-not $taskProcess.WaitForExit(90000)) {
   $taskProcess.Kill()
   throw "Client test timed out: $Flag"
  }
  if ($taskProcess.ExitCode -ne 0) { throw "Client test failed: $Flag ($($taskProcess.ExitCode))" }
  Write-Output "PASS $Flag"
 }
 Invoke-ClientTest '--self-test' ''
 Invoke-ClientTest '--selftest-exchange' 'native-clipboard.json'
 Invoke-ClientTest '--selftest-exchange-hang' 'file-panel-hang.json'
 Invoke-ClientTest '--selftest-ui' 'ui.json'
 $taskCommon = @('-std=c++20', '-O2', '-Wall', '-Wextra', '-Werror', '-static-libgcc', '-static-libstdc++')
 $taskAudioPlayout = Join-Path $taskOutput 'audio-playout.exe'
 Invoke-Checked $taskCompiler ($taskCommon + @('shared/probes/audio-playout-test.cpp', '-o', $taskAudioPlayout))
 Invoke-Checked $taskAudioPlayout @()
 $taskAudioCapture = Join-Path $taskOutput 'audio-packetizer.exe'
 Invoke-Checked $taskCompiler ($taskCommon + @('apps/macos-host/probes/audio-packetizer-test.cpp', '-o', $taskAudioCapture))
 Invoke-Checked $taskAudioCapture @()
 $taskAudioDevice = Join-Path $taskOutput 'audio-format.exe'
 Invoke-Checked $taskCompiler ($taskCommon + @('apps/windows-client/probes/audio-player.cpp', '-o', $taskAudioDevice, '-lole32', '-luuid', '-lavrt'))
 Invoke-Checked $taskAudioDevice @()
 $taskEngine = Join-Path $taskOutput 'exchange-core.exe'
 Invoke-Checked $taskCompiler ($taskCommon + @('-DREMOTE_EXCHANGE_TEST_HOOKS', 'shared/probes/exchange-test.cpp', 'shared/exchange.cpp', 'shared/diagnostic-log.cpp', '-o', $taskEngine, '-lole32', '-lcrypt32'))
 Invoke-Checked $taskEngine @()
 $taskLogger = Join-Path $taskOutput 'diagnostic-log.exe'
 Invoke-Checked $taskCompiler ($taskCommon + @('shared/probes/diagnostic-log-test.cpp', 'shared/diagnostic-log.cpp', '-o', $taskLogger, '-lshell32', '-lole32'))
 Invoke-Checked $taskLogger @((Join-Path $taskOutput ('logger-' + [guid]::NewGuid().ToString('N'))))
 $taskWatchdog = Join-Path $taskOutput 'ui-watchdog.exe'
 Invoke-Checked $taskCompiler ($taskCommon + @('shared/probes/ui-watchdog-test.cpp', 'apps/windows-client/ui-watchdog.cpp', 'shared/diagnostic-log.cpp', '-o', $taskWatchdog, '-lshell32', '-lole32'))
 Invoke-Checked $taskWatchdog @((Join-Path $taskOutput ('watchdog-' + [guid]::NewGuid().ToString('N'))))
} finally {
 if ($taskOriginalPath) { $env:PATH = $taskOriginalPath }
 Pop-Location
}
