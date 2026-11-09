#include "updater.h"
#include "../../shared/diagnostic-log.h"
#include <winhttp.h>
#include <bcrypt.h>
#include <shlobj.h>
#include <algorithm>
#include <array>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <functional>
#include <map>
#include <mutex>
#include <thread>
#include <vector>

namespace remote_update {
namespace {

constexpr const char* kReleaseUrl = "https://api.github.com/repos/dixtdf/GlideBeam/releases/latest";
constexpr const char* kDownloadPrefix = "https://github.com/dixtdf/GlideBeam/releases/download/";
constexpr size_t kMetadataLimit = 8 * 1024 * 1024;
constexpr size_t kManifestLimit = 64 * 1024;
constexpr size_t kInstallerLimit = 512 * 1024 * 1024;

std::string Architecture() {
#if defined(_M_X64) || defined(__x86_64__)
    return "x64";
#elif defined(_M_ARM64) || defined(__aarch64__)
    return "arm64";
#else
    return {};
#endif
}

struct Version {
    std::array<uint64_t, 3> core {};
    std::vector<std::string> prerelease;
};
bool Numeric(const std::string& text, uint64_t& value) {
    if (text.empty() || (text.size() > 1 && text.front() == '0')) return false;
    value = 0;
    for (char c : text) {
        if (c < '0' || c > '9') return false;
        unsigned digit = static_cast<unsigned>(c - '0');
        if (value > (UINT64_MAX - digit) / 10) return false;
        value = value * 10 + digit;
    }
    return true;
}
bool ParseVersion(const std::string& text, Version& version) {
    if (text.empty() || text.find('+') != std::string::npos) return false;
    const auto dash = text.find('-');
    const std::string core = text.substr(0, dash);
    size_t start = 0;
    for (unsigned part = 0; part < 3; ++part) {
        const auto dot = core.find('.', start);
        if ((part < 2 && dot == std::string::npos) || (part == 2 && dot != std::string::npos)) return false;
        if (!Numeric(core.substr(start, dot - start), version.core[part])) return false;
        start = dot == std::string::npos ? core.size() : dot + 1;
    }
    version.prerelease.clear();
    if (dash == std::string::npos) return true;
    const std::string pre = text.substr(dash + 1);
    if (pre.empty()) return false;
    start = 0;
    while (start <= pre.size()) {
        const auto dot = pre.find('.', start);
        const auto item = pre.substr(start, dot - start);
        if (item.empty()) return false;
        bool numeric = true;
        for (char c : item) {
            if (!std::isalnum(static_cast<unsigned char>(c)) && c != '-') return false;
            numeric = numeric && c >= '0' && c <= '9';
        }
        if (numeric && (item.size() > 1 && item.front() == '0')) return false;
        version.prerelease.push_back(item);
        if (dot == std::string::npos) break;
        start = dot + 1;
    }
    return true;
}
int ComparePrerelease(const std::string& left, const std::string& right) {
    uint64_t a = 0, b = 0;
    const bool aNumeric = Numeric(left, a), bNumeric = Numeric(right, b);
    if (aNumeric && bNumeric) return a < b ? -1 : a > b ? 1 : 0;
    if (aNumeric != bNumeric) return aNumeric ? -1 : 1;
    return left < right ? -1 : left > right ? 1 : 0;
}

void AppendUtf8(std::string& out, uint32_t cp) {
    if (cp <= 0x7f) out.push_back(static_cast<char>(cp));
    else if (cp <= 0x7ff) {
        out.push_back(static_cast<char>(0xc0 | (cp >> 6)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3f)));
    } else if (cp <= 0xffff) {
        out.push_back(static_cast<char>(0xe0 | (cp >> 12)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3f)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3f)));
    } else {
        out.push_back(static_cast<char>(0xf0 | (cp >> 18)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3f)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3f)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3f)));
    }
}

struct Json {
    enum class Type { Null, Bool, Number, String, Array, Object } type = Type::Null;
    bool boolean = false;
    std::string text;
    std::vector<Json> items;
    std::vector<std::pair<std::string, Json>> members;
    const Json* Get(const std::string& key) const {
        if (type != Type::Object) return nullptr;
        for (const auto& item : members) if (item.first == key) return &item.second;
        return nullptr;
    }
};
class JsonParser {
public:
    explicit JsonParser(const std::string& input) : p_(input.data()), end_(input.data() + input.size()) {}
    bool Parse(Json& value) {
        if (!Value(value, 0)) return false;
        Space();
        return p_ == end_;
    }
private:
    const char* p_;
    const char* end_;
    void Space() { while (p_ != end_ && (*p_ == ' ' || *p_ == '\t' || *p_ == '\r' || *p_ == '\n')) ++p_; }
    bool Take(char c) { Space(); if (p_ == end_ || *p_ != c) return false; ++p_; return true; }
    bool Literal(const char* text) {
        const size_t size = std::strlen(text);
        if (static_cast<size_t>(end_ - p_) < size || std::memcmp(p_, text, size) != 0) return false;
        p_ += size; return true;
    }
    bool Hex4(uint32_t& value) {
        if (end_ - p_ < 4) return false;
        value = 0;
        for (unsigned i = 0; i < 4; ++i) {
            const char c = *p_++;
            value <<= 4;
            if (c >= '0' && c <= '9') value |= c - '0';
            else if (c >= 'a' && c <= 'f') value |= c - 'a' + 10;
            else if (c >= 'A' && c <= 'F') value |= c - 'A' + 10;
            else return false;
        }
        return true;
    }
    bool String(std::string& out) {
        Space();
        if (p_ == end_ || *p_++ != '"') return false;
        out.clear();
        while (p_ != end_) {
            const unsigned char c = static_cast<unsigned char>(*p_++);
            if (c == '"') return true;
            if (c < 0x20) return false;
            if (c != '\\') { out.push_back(static_cast<char>(c)); continue; }
            if (p_ == end_) return false;
            switch (*p_++) {
            case '"': out.push_back('"'); break;
            case '\\': out.push_back('\\'); break;
            case '/': out.push_back('/'); break;
            case 'b': out.push_back('\b'); break;
            case 'f': out.push_back('\f'); break;
            case 'n': out.push_back('\n'); break;
            case 'r': out.push_back('\r'); break;
            case 't': out.push_back('\t'); break;
            case 'u': {
                uint32_t cp = 0;
                if (!Hex4(cp)) return false;
                if (cp >= 0xd800 && cp <= 0xdbff) {
                    if (end_ - p_ < 2 || p_[0] != '\\' || p_[1] != 'u') return false;
                    p_ += 2;
                    uint32_t low = 0;
                    if (!Hex4(low) || low < 0xdc00 || low > 0xdfff) return false;
                    cp = 0x10000 + ((cp - 0xd800) << 10) + (low - 0xdc00);
                } else if (cp >= 0xdc00 && cp <= 0xdfff) return false;
                AppendUtf8(out, cp); break;
            }
            default: return false;
            }
        }
        return false;
    }
    bool Number(Json& value) {
        const char* start = p_;
        if (p_ != end_ && *p_ == '-') ++p_;
        if (p_ == end_) return false;
        if (*p_ == '0') ++p_;
        else {
            if (*p_ < '1' || *p_ > '9') return false;
            while (p_ != end_ && *p_ >= '0' && *p_ <= '9') ++p_;
        }
        if (p_ != end_ && *p_ == '.') {
            ++p_;
            if (p_ == end_ || *p_ < '0' || *p_ > '9') return false;
            while (p_ != end_ && *p_ >= '0' && *p_ <= '9') ++p_;
        }
        if (p_ != end_ && (*p_ == 'e' || *p_ == 'E')) {
            ++p_;
            if (p_ != end_ && (*p_ == '+' || *p_ == '-')) ++p_;
            if (p_ == end_ || *p_ < '0' || *p_ > '9') return false;
            while (p_ != end_ && *p_ >= '0' && *p_ <= '9') ++p_;
        }
        value.type = Json::Type::Number; value.text.assign(start, p_ - start); return true;
    }
    bool Value(Json& value, unsigned depth) {
        if (depth > 64) return false;
        Space();
        if (p_ == end_) return false;
        if (*p_ == '"') { value.type = Json::Type::String; return String(value.text); }
        if (*p_ == 'n') { if (!Literal("null")) return false; value.type = Json::Type::Null; return true; }
        if (*p_ == 't') { if (!Literal("true")) return false; value.type = Json::Type::Bool; value.boolean = true; return true; }
        if (*p_ == 'f') { if (!Literal("false")) return false; value.type = Json::Type::Bool; value.boolean = false; return true; }
        if (*p_ == '-' || (*p_ >= '0' && *p_ <= '9')) return Number(value);
        if (*p_ == '[') {
            ++p_; value.type = Json::Type::Array; value.items.clear();
            Space(); if (p_ != end_ && *p_ == ']') { ++p_; return true; }
            while (true) {
                Json item;
                if (!Value(item, depth + 1)) return false;
                value.items.push_back(std::move(item));
                Space();
                if (p_ != end_ && *p_ == ']') { ++p_; return true; }
                if (p_ == end_ || *p_++ != ',') return false;
            }
        }
        if (*p_ == '{') {
            ++p_; value.type = Json::Type::Object; value.members.clear();
            Space(); if (p_ != end_ && *p_ == '}') { ++p_; return true; }
            while (true) {
                std::string key;
                if (!String(key) || !Take(':')) return false;
                for (const auto& member : value.members) if (member.first == key) return false;
                Json item;
                if (!Value(item, depth + 1)) return false;
                value.members.emplace_back(std::move(key), std::move(item));
                Space();
                if (p_ != end_ && *p_ == '}') { ++p_; return true; }
                if (p_ == end_ || *p_++ != ',') return false;
            }
        }
        return false;
    }
};

struct HttpHandle {
    HINTERNET value = nullptr;
    ~HttpHandle() { if (value) WinHttpCloseHandle(value); }
    operator HINTERNET() const { return value; }
};
struct Progress {
    std::function<bool()> cancelled;
    std::function<void(uint64_t, uint64_t)> advance;
};
bool HttpError(std::string& error, const char* step) {
    error = std::string(step) + " failed (Win32 " + std::to_string(GetLastError()) + ")";
    return false;
}
bool HttpGet(const std::string& url, std::vector<uint8_t>* memory,
             const std::filesystem::path* file, size_t limit,
             const Progress& progress, std::string& error) {
    if ((memory != nullptr) == (file != nullptr)) { error = "invalid download destination"; return false; }
    URL_COMPONENTSW parts {};
    parts.dwStructSize = sizeof parts;
    parts.dwSchemeLength = static_cast<DWORD>(-1);
    parts.dwHostNameLength = static_cast<DWORD>(-1);
    parts.dwUrlPathLength = static_cast<DWORD>(-1);
    parts.dwExtraInfoLength = static_cast<DWORD>(-1);
    const std::wstring wide(url.begin(), url.end());
    if (!WinHttpCrackUrl(wide.c_str(), 0, 0, &parts)) return HttpError(error, "URL parsing");
    if (parts.nScheme != INTERNET_SCHEME_HTTPS || parts.nPort != INTERNET_DEFAULT_HTTPS_PORT ||
        !parts.lpszHostName || !parts.lpszUrlPath) { error = "update URL is not HTTPS on port 443"; return false; }
    const std::wstring host(parts.lpszHostName, parts.dwHostNameLength);
    std::wstring path(parts.lpszUrlPath, parts.dwUrlPathLength);
    if (parts.lpszExtraInfo && parts.dwExtraInfoLength) path.append(parts.lpszExtraInfo, parts.dwExtraInfoLength);
    HttpHandle session {WinHttpOpen(L"GlideBeam-Updater/1.0", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
        WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0)};
    if (!session) return HttpError(error, "HTTP initialization");
    WinHttpSetTimeouts(session, 15000, 15000, 30000, 30000);
    HttpHandle connection {WinHttpConnect(session, host.c_str(), parts.nPort, 0)};
    if (!connection) return HttpError(error, "HTTP connection");
    HttpHandle request {WinHttpOpenRequest(connection, L"GET", path.c_str(), nullptr,
        WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE)};
    if (!request) return HttpError(error, "HTTP request creation");
    DWORD redirectPolicy = WINHTTP_OPTION_REDIRECT_POLICY_DISALLOW_HTTPS_TO_HTTP;
    WinHttpSetOption(request, WINHTTP_OPTION_REDIRECT_POLICY, &redirectPolicy, sizeof redirectPolicy);
    const wchar_t* headers = L"Accept: application/vnd.github+json, application/octet-stream\r\n";
    if (!WinHttpSendRequest(request, headers, static_cast<DWORD>(-1L), WINHTTP_NO_REQUEST_DATA, 0, 0, 0))
        return HttpError(error, "HTTP send");
    if (!WinHttpReceiveResponse(request, nullptr)) return HttpError(error, "HTTP response");
    DWORD status = 0, statusSize = sizeof status;
    if (!WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
        WINHTTP_HEADER_NAME_BY_INDEX, &status, &statusSize, WINHTTP_NO_HEADER_INDEX)) return HttpError(error, "HTTP status");
    if (status != HTTP_STATUS_OK) { error = "HTTP status " + std::to_string(status); return false; }
    uint64_t contentLength = 0;
    DWORD length = 0, lengthSize = sizeof length;
    if (WinHttpQueryHeaders(request, WINHTTP_QUERY_CONTENT_LENGTH | WINHTTP_QUERY_FLAG_NUMBER,
        WINHTTP_HEADER_NAME_BY_INDEX, &length, &lengthSize, WINHTTP_NO_HEADER_INDEX)) contentLength = length;
    if (contentLength > limit) { error = "download exceeds the size limit"; return false; }
    HANDLE output = INVALID_HANDLE_VALUE;
    if (file) {
        output = CreateFileW(file->c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW,
            FILE_ATTRIBUTE_NORMAL | FILE_FLAG_WRITE_THROUGH, nullptr);
        if (output == INVALID_HANDLE_VALUE) return HttpError(error, "download file creation");
    }
    bool ok = true;
    uint64_t total = 0;
    std::array<uint8_t, 65536> buffer {};
    while (ok) {
        if (progress.cancelled && progress.cancelled()) { error = "download cancelled"; ok = false; break; }
        DWORD available = 0;
        if (!WinHttpQueryDataAvailable(request, &available)) { ok = HttpError(error, "HTTP availability"); break; }
        if (!available) break;
        const DWORD wanted = static_cast<DWORD>(std::min<size_t>(available, buffer.size()));
        DWORD read = 0;
        if (!WinHttpReadData(request, buffer.data(), wanted, &read)) { ok = HttpError(error, "HTTP read"); break; }
        if (!read) break;
        if (total + read > limit || (contentLength && total + read > contentLength)) {
            error = "download exceeds its declared size"; ok = false; break;
        }
        if (memory) memory->insert(memory->end(), buffer.data(), buffer.data() + read);
        else {
            DWORD written = 0;
            if (!WriteFile(output, buffer.data(), read, &written, nullptr) || written != read) {
                ok = HttpError(error, "download file write"); break;
            }
        }
        total += read;
        if (progress.advance) progress.advance(total, contentLength);
    }
    if (contentLength && total != contentLength) { error = "truncated download"; ok = false; }
    if (file) {
        if (!CloseHandle(output) && ok) { error = "download file close failed"; ok = false; }
        if (!ok) DeleteFileW(file->c_str());
    }
    if (ok && memory && total != memory->size()) { error = "download size mismatch"; ok = false; }
    return ok;
}

std::string HexLower(const uint8_t* bytes, size_t size) {
    static constexpr char digits[] = "0123456789abcdef";
    std::string out(size * 2, '0');
    for (size_t i = 0; i < size; ++i) {
        out[i * 2] = digits[bytes[i] >> 4];
        out[i * 2 + 1] = digits[bytes[i] & 15];
    }
    return out;
}

std::filesystem::path UpdateDirectory(const ReleaseInfo& info) {
    PWSTR local = nullptr;
    if (FAILED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &local))) return {};
    std::filesystem::path root(local);
    CoTaskMemFree(local);
    root /= L"GlideBeam";
    root /= L"updates";
    root /= std::filesystem::path(info.version + "-" + info.architecture);
    return root;
}

} // namespace

int CompareVersions(const std::string& left, const std::string& right) {
    Version a, b;
    if (!ParseVersion(left, a) || !ParseVersion(right, b)) return -2;
    for (unsigned i = 0; i < 3; ++i) if (a.core[i] != b.core[i]) return a.core[i] < b.core[i] ? -1 : 1;
    if (a.prerelease.empty() != b.prerelease.empty()) return a.prerelease.empty() ? 1 : -1;
    const size_t common = std::min(a.prerelease.size(), b.prerelease.size());
    for (size_t i = 0; i < common; ++i) {
        const int result = ComparePrerelease(a.prerelease[i], b.prerelease[i]);
        if (result) return result;
    }
    if (a.prerelease.size() == b.prerelease.size()) return 0;
    return a.prerelease.size() < b.prerelease.size() ? -1 : 1;
}

std::string ExpectedAssetName(const std::string& version, const std::string& architecture) {
    return "GlideBeam-" + version + "-Windows-" + architecture + "-Setup.exe";
}

std::string ExpectedSha256(const std::string& manifest, const std::string& assetName) {
    std::string result;
    size_t start = 0;
    while (start <= manifest.size()) {
        const auto end = manifest.find('\n', start);
        auto line = manifest.substr(start, end - start);
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (!line.empty()) {
            if (line.size() < 67 || line[64] != ' ' || line[65] != ' ') return {};
            for (size_t i = 0; i < 64; ++i) if (!std::isxdigit(static_cast<unsigned char>(line[i]))) return {};
            if (line.substr(66) == assetName) {
                if (!result.empty()) return {};
                result = line.substr(0, 64);
                std::transform(result.begin(), result.end(), result.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            }
        }
        if (end == std::string::npos) break;
        start = end + 1;
    }
    return result;
}

bool ParseRelease(const std::string& json, const std::string& architecture,
                  ReleaseInfo& info, std::string& error) {
    Json root;
    if (!JsonParser(json).Parse(root) || root.type != Json::Type::Object) { error = "invalid release JSON"; return false; }
    const Json* tag = root.Get("tag_name");
    const Json* draft = root.Get("draft");
    const Json* prerelease = root.Get("prerelease");
    const Json* assets = root.Get("assets");
    if (!tag || tag->type != Json::Type::String || tag->text.size() < 2 || tag->text.front() != 'v' ||
        !draft || draft->type != Json::Type::Bool || draft->boolean ||
        !prerelease || prerelease->type != Json::Type::Bool || prerelease->boolean ||
        !assets || assets->type != Json::Type::Array) { error = "release is not a stable published version"; return false; }
    Version parsed;
    if (!ParseVersion(tag->text.substr(1), parsed)) { error = "invalid release tag"; return false; }
    ReleaseInfo selected;
    selected.version = tag->text.substr(1);
    selected.architecture = architecture;
    selected.assetName = ExpectedAssetName(selected.version, architecture);
    const std::string base = std::string(kDownloadPrefix) + tag->text + "/";
    for (const auto& asset : assets->items) {
        const Json* name = asset.Get("name");
        const Json* url = asset.Get("browser_download_url");
        if (!name || name->type != Json::Type::String || !url || url->type != Json::Type::String) continue;
        if (name->text != selected.assetName && name->text != "SHA256SUMS.txt") continue;
        if (url->text != base + name->text) { error = "unexpected release asset URL"; return false; }
        std::string* destination = name->text == selected.assetName ? &selected.assetUrl : &selected.sumsUrl;
        if (!destination->empty()) { error = "duplicate release asset"; return false; }
        *destination = url->text;
    }
    if (selected.assetUrl.empty() || selected.sumsUrl.empty()) { error = "release lacks Setup or SHA256SUMS.txt for this architecture"; return false; }
    info = std::move(selected);
    return true;
}

std::string Sha256File(const std::filesystem::path& path) {
    HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (file == INVALID_HANDLE_VALUE) return {};
    BCRYPT_ALG_HANDLE algorithm = nullptr;
    BCRYPT_HASH_HANDLE hash = nullptr;
    std::string result;
    bool ok = BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) >= 0 &&
        BCryptCreateHash(algorithm, &hash, nullptr, 0, nullptr, 0, 0) >= 0;
    std::array<uint8_t, 65536> buffer {};
    while (ok) {
        DWORD read = 0;
        if (!ReadFile(file, buffer.data(), static_cast<DWORD>(buffer.size()), &read, nullptr)) { ok = false; break; }
        if (!read) break;
        if (BCryptHashData(hash, buffer.data(), read, 0) < 0) { ok = false; break; }
    }
    std::array<uint8_t, 32> digest {};
    if (ok && BCryptFinishHash(hash, digest.data(), static_cast<ULONG>(digest.size()), 0) >= 0)
        result = HexLower(digest.data(), digest.size());
    if (hash) BCryptDestroyHash(hash);
    if (algorithm) BCryptCloseAlgorithmProvider(algorithm, 0);
    CloseHandle(file);
    return result;
}

std::string CurrentVersion() {
    std::vector<wchar_t> path(32768);
    const DWORD length = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
    if (!length || length >= path.size()) return {};
    DWORD ignored = 0;
    const DWORD size = GetFileVersionInfoSizeW(path.data(), &ignored);
    if (!size) return {};
    std::vector<uint8_t> data(size);
    if (!GetFileVersionInfoW(path.data(), 0, size, data.data())) return {};
    wchar_t* value = nullptr;
    UINT valueSize = 0;
    if (!VerQueryValueW(data.data(), L"\\StringFileInfo\\040904b0\\ProductVersion",
        reinterpret_cast<void**>(&value), &valueSize) || !value || !valueSize) return {};
    std::wstring wide(value, valueSize);
    while (!wide.empty() && (wide.back() == L'\0' || wide.back() == L' ' || wide.back() == L'\r' || wide.back() == L'\n')) wide.pop_back();
    return std::string(wide.begin(), wide.end());
}

struct Updater::State {
    mutable std::mutex mutex;
    Snapshot snapshot;
    bool busy = false;
    bool cancel = false;
    ReleaseInfo release;
    std::string expectedSha;
};

void Updater::Fail(const std::shared_ptr<Updater::State>& state, const std::string& error) {
    std::lock_guard lock(state->mutex);
    state->snapshot.stage = Stage::Error;
    state->snapshot.error = error;
    state->snapshot.installerPath.clear();
    state->busy = false;
    ++state->snapshot.revision;
    remote_log::Write(remote_log::Level::Warning, "updater", error);
}

Updater::Updater() : state_(std::make_shared<State>()) {
    state_->snapshot.currentVersion = CurrentVersion();
    state_->snapshot.revision = 1;
}
Updater::~Updater() {
    std::lock_guard lock(state_->mutex);
    state_->cancel = true;
}
Snapshot Updater::GetSnapshot() const {
    std::lock_guard lock(state_->mutex);
    return state_->snapshot;
}

void Updater::Check() {
    {
        std::lock_guard lock(state_->mutex);
        if (state_->busy) return;
        state_->busy = true;
        state_->cancel = false;
        state_->snapshot.stage = Stage::Checking;
        state_->snapshot.error.clear();
        state_->snapshot.downloadedBytes = state_->snapshot.totalBytes = 0;
        state_->snapshot.installerPath.clear();
        ++state_->snapshot.revision;
    }
    const auto state = state_;
    std::thread([state] {
        std::vector<uint8_t> bytes;
        std::string error;
        Progress progress {[state] { std::lock_guard lock(state->mutex); return state->cancel; }, {}};
        if (!HttpGet(kReleaseUrl, &bytes, nullptr, kMetadataLimit, progress, error)) { Fail(state, error); return; }
        ReleaseInfo info;
        if (!ParseRelease(std::string(bytes.begin(), bytes.end()), Architecture(), info, error)) { Fail(state, error); return; }
        const int comparison = CompareVersions(state->snapshot.currentVersion, info.version);
        if (comparison == -2) { Fail(state, "invalid current application version"); return; }
        {
            std::lock_guard lock(state->mutex);
            state->release = info;
            state->expectedSha.clear();
            state->snapshot.availableVersion = info.version;
            state->snapshot.stage = comparison < 0 ? Stage::Available : Stage::UpToDate;
            state->busy = false;
            ++state->snapshot.revision;
        }
        remote_log::Write(remote_log::Level::Info, "updater", "release check completed");
    }).detach();
}

void Updater::Download() {
    ReleaseInfo info;
    {
        std::lock_guard lock(state_->mutex);
        if (state_->busy || state_->snapshot.stage != Stage::Available) return;
        state_->busy = true;
        state_->cancel = false;
        state_->snapshot.stage = Stage::Downloading;
        state_->snapshot.error.clear();
        state_->snapshot.downloadedBytes = state_->snapshot.totalBytes = 0;
        ++state_->snapshot.revision;
        info = state_->release;
    }
    const auto state = state_;
    std::thread([state, info] {
        std::string error;
        Progress progress {[state] { std::lock_guard lock(state->mutex); return state->cancel; }, {}};
        std::vector<uint8_t> manifest;
        if (!HttpGet(info.sumsUrl, &manifest, nullptr, kManifestLimit, progress, error)) { Fail(state, error); return; }
        const std::string expected = ExpectedSha256(std::string(manifest.begin(), manifest.end()), info.assetName);
        if (expected.empty()) { Fail(state, "release manifest has no unique checksum for the selected Setup"); return; }
        const auto directory = UpdateDirectory(info);
        if (directory.empty()) { Fail(state, "cannot locate the per-user update directory"); return; }
        std::error_code filesystemError;
        std::filesystem::create_directories(directory, filesystemError);
        if (filesystemError) { Fail(state, "cannot create the per-user update directory"); return; }
        const auto part = directory / std::filesystem::path(info.assetName + ".part");
        const auto finalPath = directory / std::filesystem::path(info.assetName);
        DeleteFileW(part.c_str());
        Progress downloadProgress {[state] { std::lock_guard lock(state->mutex); return state->cancel; },
            [state](uint64_t downloaded, uint64_t total) {
                std::lock_guard lock(state->mutex);
                state->snapshot.downloadedBytes = downloaded;
                state->snapshot.totalBytes = total;
                ++state->snapshot.revision;
            }};
        if (!HttpGet(info.assetUrl, nullptr, &part, kInstallerLimit, downloadProgress, error)) { Fail(state, error); return; }
        if (Sha256File(part) != expected) { DeleteFileW(part.c_str()); Fail(state, "Setup SHA-256 mismatch"); return; }
        if (!MoveFileExW(part.c_str(), finalPath.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
            DeleteFileW(part.c_str()); Fail(state, "cannot finalize the downloaded Setup"); return;
        }
        {
            std::lock_guard lock(state->mutex);
            state->expectedSha = expected;
            state->snapshot.installerPath = finalPath;
            state->snapshot.stage = Stage::Ready;
            state->busy = false;
            ++state->snapshot.revision;
        }
        remote_log::Write(remote_log::Level::Info, "updater", "Setup downloaded and SHA-256 verified");
    }).detach();
}

bool Updater::Install(HWND owner) {
    std::filesystem::path installer;
    std::string expected;
    {
        std::lock_guard lock(state_->mutex);
        if (state_->busy || state_->snapshot.stage != Stage::Ready) return false;
        state_->busy = true;
        state_->snapshot.stage = Stage::Installing;
        state_->snapshot.error.clear();
        ++state_->snapshot.revision;
        installer = state_->snapshot.installerPath;
        expected = state_->expectedSha;
    }
    if (installer.empty() || expected.empty() || Sha256File(installer) != expected) {
        Fail(state_, "Setup changed after verification; download it again");
        return false;
    }
    const auto logPath = installer.parent_path() / L"setup.log";
    const auto workingDirectory = installer.parent_path();
    const std::wstring arguments = L"/VERYSILENT /SUPPRESSMSGBOXES /NORESTART /SP- /CLOSEAPPLICATIONS /NORESTARTAPPLICATIONS /LOG=\"" + logPath.wstring() + L"\"";
    SHELLEXECUTEINFOW execute {};
    execute.cbSize = sizeof execute;
    execute.fMask = SEE_MASK_NOCLOSEPROCESS | SEE_MASK_FLAG_NO_UI;
    execute.hwnd = owner;
    execute.lpVerb = L"open";
    execute.lpFile = installer.c_str();
    execute.lpParameters = arguments.c_str();
    execute.lpDirectory = workingDirectory.c_str();
    execute.nShow = SW_SHOWNORMAL;
    if (!ShellExecuteExW(&execute)) {
        Fail(state_, "Setup launch failed (Win32 " + std::to_string(GetLastError()) + ")");
        return false;
    }
    if (execute.hProcess) CloseHandle(execute.hProcess);
    remote_log::Write(remote_log::Level::Info, "updater", "verified Setup started; client will close for replacement");
    return true;
}

bool RunUpdaterSelfTest(const wchar_t* reportPath) {
    std::vector<std::pair<std::string, bool>> checks;
    auto check = [&](const char* name, bool value) { checks.emplace_back(name, value); };
    check("version_orders_numeric_components", CompareVersions("1.2.9", "1.10.0") == -1 && CompareVersions("2.0.0", "1.99.99") == 1);
    check("version_release_outranks_prerelease", CompareVersions("1.0.0-beta.11", "1.0.0") == -1 && CompareVersions("1.0.0", "1.0.0-rc.1") == 1);
    check("version_prerelease_numeric_and_alpha_rules", CompareVersions("1.0.0-beta.2", "1.0.0-beta.11") == -1 &&
        CompareVersions("1.0.0-alpha", "1.0.0-alpha.1") == -1 && CompareVersions("1.0.0-1", "1.0.0-alpha") == -1);
    check("version_rejects_invalid_input", CompareVersions("v1.0.0", "1.0.0") == -2 && CompareVersions("01.0.0", "1.0.0") == -2 &&
        CompareVersions("1.0", "1.0.0") == -2 && CompareVersions("1.0.0+build", "1.0.0") == -2);
    check("asset_name_matches_release_convention", ExpectedAssetName("1.2.3", "x64") == "GlideBeam-1.2.3-Windows-x64-Setup.exe");
    const std::string hashA(64, 'a'), hashB(64, 'b');
    check("manifest_selects_exact_asset", ExpectedSha256(hashA + "  other.zip\n" + hashB + "  GlideBeam-1.2.3-Windows-x64-Setup.exe\n",
        "GlideBeam-1.2.3-Windows-x64-Setup.exe") == hashB);
    check("manifest_rejects_duplicate_or_malformed_entries", ExpectedSha256(hashA + "  GlideBeam-1.2.3-Windows-x64-Setup.exe\n" +
        hashB + "  GlideBeam-1.2.3-Windows-x64-Setup.exe\n", "GlideBeam-1.2.3-Windows-x64-Setup.exe").empty() &&
        ExpectedSha256("not-a-hash  file.exe\n", "file.exe").empty());
    const std::string asset = ExpectedAssetName("1.2.0", "x64");
    const std::string base = std::string(kDownloadPrefix) + "v1.2.0/";
    const std::string release = "{\"tag_name\":\"v1.2.0\",\"draft\":false,\"prerelease\":false,\"assets\":["
        "{\"name\":\"" + asset + "\",\"browser_download_url\":\"" + base + asset + "\"},"
        "{\"name\":\"SHA256SUMS.txt\",\"browser_download_url\":\"" + base + "SHA256SUMS.txt\"},"
        "{\"name\":\"GlideBeam-1.2.0-Windows-arm64-Setup.exe\",\"browser_download_url\":\"" + base + "GlideBeam-1.2.0-Windows-arm64-Setup.exe\"}]}";
    ReleaseInfo info;
    std::string error;
    check("release_selects_matching_architecture_and_manifest", ParseRelease(release, "x64", info, error) &&
        info.version == "1.2.0" && info.assetName == asset && info.sumsUrl == base + "SHA256SUMS.txt");
    check("release_rejects_missing_architecture", !ParseRelease(release, "arm32", info, error));
    const std::string draft = "{\"tag_name\":\"v1.2.0\",\"draft\":true,\"prerelease\":false,\"assets\":[]}";
    check("release_rejects_draft", !ParseRelease(draft, "x64", info, error));
    const std::string duplicate = "{\"tag_name\":\"v1.2.0\",\"draft\":false,\"prerelease\":false,\"assets\":["
        "{\"name\":\"" + asset + "\",\"browser_download_url\":\"" + base + asset + "\"},"
        "{\"name\":\"" + asset + "\",\"browser_download_url\":\"" + base + asset + "\"}]}";
    check("release_rejects_duplicate_assets", !ParseRelease(duplicate, "x64", info, error));
    check("release_rejects_unexpected_asset_host", !ParseRelease(
        "{\"tag_name\":\"v1.2.0\",\"draft\":false,\"prerelease\":false,\"assets\":[{\"name\":\"" + asset +
        "\",\"browser_download_url\":\"https://example.invalid/" + asset + "\"}]}", "x64", info, error));
    const auto hashPath = std::filesystem::path(reportPath).parent_path() / L"updater-hash-fixture.txt";
    {
        std::ofstream output(hashPath, std::ios::binary | std::ios::trunc);
        output << "abc";
    }
    check("sha256_matches_known_vector", Sha256File(hashPath) == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    std::error_code removeError;
    std::filesystem::remove(hashPath, removeError);
    const std::string current = CurrentVersion();
    check("current_product_version_is_semantic", CompareVersions(current, current) == 0);
    const bool passed = !checks.empty() && std::all_of(checks.begin(), checks.end(), [](const auto& item) { return item.second; });
    std::string report = "{\n  \"passed\": " + std::string(passed ? "true" : "false") +
        ",\n  \"network_used\": false,\n  \"setup_executed\": false,\n  \"checks\": {\n";
    for (size_t i = 0; i < checks.size(); ++i)
        report += "    \"" + checks[i].first + "\": " + (checks[i].second ? "true" : "false") + (i + 1 == checks.size() ? "\n" : ",\n");
    report += "  }\n}\n";
    FILE* file = _wfopen(reportPath, L"wb");
    if (!file) return false;
    const bool written = std::fwrite(report.data(), 1, report.size(), file) == report.size();
    return std::fclose(file) == 0 && written && passed;
}

} // namespace remote_update
