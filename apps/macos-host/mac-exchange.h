#pragma once
#import <AppKit/AppKit.h>
#include "../../shared/exchange.h"
#include <functional>
#include <memory>

// All methods below run on AppKit's main thread. File IO and wire work belong
// to Session's worker and the transport pump, never this native adapter.
class RemoteMacExchange {
public:
    using Paste = std::function<void(uint32_t)>;
    RemoteMacExchange(remote_exchange::Session& session, Paste paste,
                      NSPasteboard *pasteboard = nil, NSString *cacheDirectory = nil);
    ~RemoteMacExchange();
    void Tick(bool transportReady = true);
    void SetConnected(bool connected);
    void SetEnabled(bool enabled);
    bool Enabled() const;
    void RequestPaste(uint64_t clipboardId, uint32_t flags);
    void CancelPaste(NSString *reason = nil);
    void ShowTransfers();
    NSString *Status() const;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// Synthetic/private pasteboard probe calls these same production functions.
bool RemoteMacInstallClipboard(NSPasteboard *pasteboard, const remote_exchange::ClipboardPacket& packet,
                               NSString *origin);
bool RemoteMacInstallFiles(NSPasteboard *pasteboard, const std::vector<remote_exchange::fs::path>& files,
                           NSString *origin);
std::vector<remote_exchange::fs::path> RemoteMacClipboardFiles(NSPasteboard *pasteboard);
bool RemoteMacRenderTransfersPreview(remote_exchange::Session& session, NSString *path);
