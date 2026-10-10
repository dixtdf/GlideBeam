# Clipboard and file transfer

Reveal the top toolbar by moving the pointer to the top center of the connected remote window.

The Windows **Clipboard** switch controls local text/image/file clipboard participation. Mac **Clipboard and file sharing** controls the host sharing service. Disabling sharing pauses active tasks; after re-enabling, resume them from the transfer list.

## Clipboard

Unicode text and images sync in both directions. PNG and system bitmap content are supported; Mac TIFF input is converted to PNG. One content item is limited to 32 MiB. Rich text is not synchronized.

Copying files publishes a manifest first; file content starts transferring when pasted. Source files are not deleted. Windows uses native virtual-file clipboard objects. Mac downloads the file cache, then gives Finder real file URLs for its paste action. The Mac clipboard cache is `~/Library/Caches/RemoteNative/Clipboard`; it can be cleared manually when no paste needs it.

## Transfer panel and drag/drop

**File transfer** opens the Windows panel with local files on the left and Mac files on the right. Browse paths, select files/folders to send, or select remote files to download. Dropping local files on the remote video uploads them to the configured Mac receive folder; the app does not infer a Finder destination from pixels. The Mac file window also supports choosing and dropping files to send to Windows.

Default receive folders are `Downloads/Remote Native` on Windows and `~/Downloads/Remote Native` on Mac. The Mac window can change its receive folder. These legacy folder names are retained for existing installations. Protected folders remain subject to OS file access permissions.

Tasks show progress and speed and support pause, resume, and cancel. A name collision keeps both files. Each file passes SHA-256 verification before placement in the destination. Cancel removes only the unfinished data for that receive task, preserving source files and existing destination files. Symbolic links, Windows junctions, path traversal, and incompatible file names are rejected. A manifest is limited to 20,000 entries and 4 MiB of names.

Disconnected tasks can resume after reconnecting while both apps and their in-memory task state remain alive. There is no persisted task database; after quitting an app, select and send again. A changed source file invalidates resume and must be selected again.

Input, video, and file traffic use independent transport scheduling. File throughput adapts to network RTT to favor desktop response, while still sharing the machine's network and CPU resources. No zero-impact guarantee is made for every load.

The feature is on-demand two-way transfer. Directory mirroring, continuous automatic synchronization, cross-computer cut/delete, and an Internet relay are not implemented. Native clipboard fixtures and isolated transfer tests complement actual Finder/Explorer acceptance; real application behavior must be checked on the intended machines.
