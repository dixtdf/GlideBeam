# Logs, Mac restart, and disconnect behavior

Windows: open the remote toolbar's **More → Open Windows log folder**. Mac: use **Open logs** in the host window. The corresponding Chinese labels are「更多 → 打开 Windows 日志目录」and「打开日志」.

| Computer | Log folder |
| --- | --- |
| Windows | `%LOCALAPPDATA%\RemoteNative\Logs` |
| Mac | `~/Library/Logs/RemoteNative` |

Logs append across restarts and retain the current file plus two rotated files, each bounded to 4 MiB. They record UTC time, process/thread identifiers, connection and transfer state, error categories, and restart request state. Tokens, passwords, clipboard contents, and user file names are not recorded. A bounded background queue writes the files; overload drops diagnostic entries instead of making the UI wait for disk.

The Windows UI watchdog records an unresponsive heartbeat after five seconds, its current phase, and recovery. Clipboard reads, image conversion, OLE setup, and file reads have duration metadata to help distinguish blocking paths. Diagnostics do not read desktop images or process memory.

## Restart Mac

While connected, choose **More → Restart Mac…**, confirm in Windows, and save your work before authorizing the Mac system prompt. The host uses a fixed restart command and native administrator authorization. You can cancel. macOS may cache authorization briefly, so a password dialog need not appear on every request.

The app does not store an administrator password, modify sudo permissions, or accept arbitrary shell commands. Requests use UUIDs and pending duplicates do not execute repeatedly. Windows shows authorizing, submitted, cancelled, denied, and error states. A waiting status overlay can be dismissed to interact with the Mac authorization window.

Restart interrupts the connection. Disconnection during authorization or restart does not by itself prove whether the machine restarted; reconnect after the Mac returns. **Start at login** runs the agent after the same user logs in, not before login.

## Lock screen and disconnect

Disconnecting releases pressed remote keys and mouse buttons and closes the session. It does not issue a Mac lock command or change screen saver, sleep, or password policy. If the Mac locks while idle, check the current user's **System Settings → Lock Screen** and power settings. In a VM whose host identity changes, previously stored host-specific preferences may need to be checked again.

## Investigating a UI freeze

Closing the file panel hides its window while transfers continue. Clipboard/provider operations, image conversion, directory scans, and remote file streams are dispatched away from the main UI path where possible. Paste preparation is cancelled after a timeout, lost focus, or subsequent input, preventing delayed paste into another target.

If a freeze recurs, preserve both log folders and the UTC/local time of the event, and describe which window or clipboard source was involved. Logs and simulated slow-provider tests can identify candidate blocking paths; a prior event without a thread dump cannot establish a unique historical cause.
