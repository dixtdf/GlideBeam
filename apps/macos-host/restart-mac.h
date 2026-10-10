#pragma once
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>

// Only feed messages dequeued from the authenticated REMOTE_CONTROL channel.
// JSON request (<= 1024 bytes):
// {"type":"host-command","kind":"restart-mac","request_id":"<UUID>","confirmed":true}
// JSON reply: {"type":"host-command-status","kind":"restart-mac",
// "request_id":"<same UUID>","state":"authorizing|requested|cancelled|denied|error|busy",
// "message":"<fixed Chinese text>","code":"<fixed machine-readable code>"}.
// confirmed means the Windows client already showed its destructive-action
// confirmation. macOS separately requests native administrator authorization.
// Unknown commands, extra command arguments and unauthenticated calls never run.
class RemoteMacRestart {
public:
    using Send = std::function<void(const uint8_t*, size_t)>;
    explicit RemoteMacRestart(Send send);
    ~RemoteMacRestart();
    void SetConnected(bool connected);
    bool Handle(const uint8_t* json, size_t size);
    bool Busy() const;
    // Always injects fake runners. Never launches osascript, prompts or reboots.
    static int SelfTest();
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
