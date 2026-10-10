#pragma once
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace remote_exchange {
namespace fs = std::filesystem;
enum class ClipboardKind : uint32_t { Text = 1, Png = 2 };
struct ClipboardPacket { uint64_t id = 0; ClipboardKind kind = ClipboardKind::Text; std::vector<uint8_t> data; };
struct FileEntry { std::string name; uint64_t size = 0; bool directory = false; };
struct FileOffer { uint64_t id = 0; std::vector<FileEntry> entries; uint64_t bytes = 0; bool clipboard = true; };
enum class TransferState { Queued, Running, Paused, Complete, Cancelled, Error };
struct Transfer { uint64_t id = 0, offerId = 0; bool outgoing = false; TransferState state = TransferState::Queued; std::string name, error; uint64_t bytes = 0, total = 0; double bytesPerSecond = 0; fs::path destination; std::vector<fs::path> files; };
struct CompletedFiles { uint64_t transferId = 0, offerId = 0; std::vector<fs::path> files; };
struct Directory { uint64_t requestId = 0; std::string path, error; std::vector<FileEntry> entries; };
struct Snapshot { bool connected = false, enabled = true; uint64_t clipboardAcknowledged = 0, clipboardRejected = 0; FileOffer remoteOffer; std::vector<Transfer> transfers; std::string message; };
// Send returns 1 when queued, 0 for backpressure, -1 if unavailable. Called only
// from the engine worker. Native clipboard and UI access stay on their own thread.
using Send = std::function<int(const uint8_t*, size_t)>;
class Session {
public:
    Session(bool host, fs::path receiveDirectory, Send send);
    ~Session();
    Session(const Session&) = delete; Session& operator=(const Session&) = delete;
    void SetConnected(bool connected);
    void SetEnabled(bool enabled);
    void SetReceiveDirectory(fs::path directory);
    fs::path ReceiveDirectory() const;
    void SetNetworkRtt(double milliseconds);
    void SetSpeedLimit(uint64_t bytesPerSecond); // 0: adaptive operation-first default
    bool Receive(const uint8_t* data, size_t size); // bounded copy; no file IO
    uint64_t PublishClipboard(ClipboardKind kind, std::vector<uint8_t> data);
    bool TakeClipboard(ClipboardPacket& packet);
    void ClipboardInstalled(uint64_t id, bool success);
    // Offer creation and directory traversal run asynchronously on the worker.
    // Source paths never go on the wire; only names relative to the offered roots.
    uint64_t OfferFiles(std::vector<fs::path> paths);
    uint64_t RequestFiles(uint64_t offerId, fs::path destination = {});
    uint64_t SendFiles(std::vector<fs::path> paths, std::string remoteDirectory = {});
    uint64_t DownloadFiles(std::vector<std::string> remotePaths, fs::path destination = {});
    uint64_t Browse(std::string remoteDirectory = {});
    bool TakeDirectory(Directory& directory);
    bool TakeCompletedFiles(CompletedFiles& files);
    void Pause(uint64_t transferId);
    void Resume(uint64_t transferId);
    void Cancel(uint64_t transferId);
    Snapshot GetSnapshot() const;
    // Used by Windows asynchronous virtual-file streams. Returns bytes read,
    // 0 at EOF, -1 failed/cancelled, -2 timeout. Must never run on the UI thread.
    int ReadTransferFile(uint64_t transferId, const std::string& relativeName,
                         uint64_t offset, uint8_t* data, size_t capacity,
                         uint32_t timeoutMs = 30000);
private:
    struct Impl; std::unique_ptr<Impl> impl_;
};
const char* StateLabel(TransferState state);
}
