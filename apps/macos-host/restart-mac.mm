#import <Foundation/Foundation.h>
#include "restart-mac.h"
#import "localization.h"
#include "../../shared/diagnostic-log.h"
#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <deque>
#include <fcntl.h>
#include <map>
#include <mutex>
#include <spawn.h>
#include <signal.h>
#include <string>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>
#include <vector>

namespace {
using Clock = std::chrono::steady_clock;
struct Result { const char* state; const char* code; const char* message; };
static constexpr Result kRequested{"requested","restart_requested","Mac 已接受系统重启指令，连接即将断开。"};
static constexpr Result kCancelled{"cancelled","authorization_cancelled","重启已取消，Mac 将继续运行。"};
static constexpr Result kDenied{"denied","authorization_denied","Mac 未授予管理员权限，未执行重启。"};
static constexpr Result kTimeout{"error","authorization_timeout","等待 Mac 系统授权超时，未执行重启。"};
static constexpr Result kError{"error","restart_failed","Mac 重启指令执行失败，请查看 Mac 端日志。"};

// No request field is ever inserted into this script. The administrator password
// is collected exclusively by macOS and never reaches the app or its logs.
static constexpr const char kRestartScript[] =
    "try\n"
    "do shell script \"/sbin/shutdown -r now\" with administrator privileges\n"
    "return \"REMOTE_RESTART_REQUESTED\"\n"
    "on error number n\n"
    "if n is -128 or n is -60006 then\nreturn \"REMOTE_RESTART_CANCELLED\"\n"
    "else if n is -60005 or n is -60007 then\nreturn \"REMOTE_RESTART_DENIED\"\n"
    "else\nreturn \"REMOTE_RESTART_ERROR\"\nend if\nend try";

static std::string RestartAuthorizationScript() {
    std::string script(kRestartScript);
    const char *prompt = remote_i18n::EffectiveLanguage() == remote_i18n::Language::SimplifiedChinese
        ? " with prompt \"光梭将重启这台 Mac。请先保存工作；授权后立即重启。\""
        : " with prompt \"GlideBeam will restart this Mac. Save your work; the Mac restarts immediately after authorization.\"";
    const std::string clause = "with administrator privileges";
    script.insert(script.find(clause) + clause.size(), prompt);
    return script;
}
static Result RunNativeAuthorization(const std::atomic<bool>& cancelled) {
    int pipes[2];
    if (pipe(pipes) != 0) return kError;
    fcntl(pipes[0], F_SETFL, fcntl(pipes[0], F_GETFL) | O_NONBLOCK);
    fcntl(pipes[0], F_SETFD, FD_CLOEXEC);
    fcntl(pipes[1], F_SETFD, FD_CLOEXEC);
    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_adddup2(&actions, pipes[1], STDOUT_FILENO);
    // Raw authorization/AppleScript diagnostics may include account names.
    posix_spawn_file_actions_addopen(&actions, STDERR_FILENO, "/dev/null", O_WRONLY, 0);
    posix_spawn_file_actions_addopen(&actions, STDIN_FILENO, "/dev/null", O_RDONLY, 0);
    posix_spawn_file_actions_addclose(&actions, pipes[0]);
    posix_spawn_file_actions_addclose(&actions, pipes[1]);
    const auto script = RestartAuthorizationScript();
    char* args[] = {const_cast<char*>("/usr/bin/osascript"), const_cast<char*>("-e"),
                    const_cast<char*>(script.c_str()), nullptr};
    char* environment[] = {const_cast<char*>("PATH=/usr/bin:/bin:/usr/sbin:/sbin"), nullptr};
    pid_t pid = 0;
    int launched = cancelled.load() ? ECANCELED : posix_spawn(&pid, args[0], &actions, nullptr, args, environment);
    posix_spawn_file_actions_destroy(&actions);
    close(pipes[1]);
    if (launched != 0) {
        close(pipes[0]);
        if (!cancelled.load()) remote_log::Write(remote_log::Level::Error,"restart","native authorization launch failed");
        return cancelled.load() ? kCancelled : kError;
    }
    std::string output;
    auto deadline = Clock::now() + std::chrono::seconds(120);
    bool stopped = false, timeout = false;
    int status = 0;
    auto killAt = Clock::time_point::max();
    for (;;) {
        char buffer[128];
        ssize_t count;
        while ((count = read(pipes[0], buffer, sizeof(buffer))) > 0)
            if (output.size() < 1024) output.append(buffer, std::min<size_t>(count, 1024 - output.size()));
        pid_t waited = waitpid(pid, &status, WNOHANG);
        if (waited == pid) break;
        if (waited < 0 && errno != EINTR) { close(pipes[0]); return kError; }
        auto now = Clock::now();
        if (!stopped && (cancelled.load() || now >= deadline)) {
            timeout = !cancelled.load(); stopped = true; kill(pid, SIGTERM);
            killAt = now + std::chrono::seconds(1);
        } else if (stopped && now >= killAt) {
            kill(pid, SIGKILL); killAt = Clock::time_point::max();
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(25));
    }
    // Collect the final marker even when the child completed between polls.
    char buffer[128]; ssize_t count;
    while ((count = read(pipes[0], buffer, sizeof(buffer))) > 0)
        if (output.size() < 1024) output.append(buffer, std::min<size_t>(count, 1024 - output.size()));
    close(pipes[0]);
    if (stopped || cancelled.load()) return timeout ? kTimeout : kCancelled;
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) return kError;
    if (output == "REMOTE_RESTART_REQUESTED\n") return kRequested;
    if (output == "REMOTE_RESTART_CANCELLED\n") return kCancelled;
    if (output == "REMOTE_RESTART_DENIED\n") return kDenied;
    return kError;
}

static NSData* Reply(NSString* id, Result result) {
    return [NSJSONSerialization dataWithJSONObject:@{@"type":@"host-command-status", @"kind":@"restart-mac",
        @"request_id":id ?: @"", @"state":[NSString stringWithUTF8String:result.state],
        @"code":[NSString stringWithUTF8String:result.code], @"message":RemoteHostText([NSString stringWithUTF8String:result.message])}
        options:0 error:nil];
}
}

struct RemoteMacRestart::Impl {
    using Runner = std::function<Result(const std::atomic<bool>&)>;
    Send send;
    Runner run;
    mutable std::mutex mutex;
    std::map<std::string, NSData*> replies;
    std::deque<std::string> order;
    std::thread worker;
    std::atomic<bool> cancelled{false};
    bool connected = false, busy = false;
    std::string active;
    explicit Impl(Send s, Runner r) : send(std::move(s)), run(std::move(r)) {}
    ~Impl() { cancelled.store(true); if (worker.joinable()) worker.join(); }
    void emit(NSData* data) { if (data && send) send((const uint8_t*)data.bytes, data.length); }
    void remember(const std::string& key, NSData* data) {
        if (!replies.count(key)) order.push_back(key);
        replies[key] = data;
        while (order.size() > 128) {
            if (order.front() == active) { order.push_back(order.front()); order.pop_front(); }
            else { replies.erase(order.front()); order.pop_front(); }
        }
    }
    void connection(bool value) {
        std::lock_guard lock(mutex);
        connected = value;
        if (!value && busy) cancelled.store(true);
    }
    bool handle(const uint8_t* bytes, size_t size) {
        if (!bytes || size == 0 || size > 1024) return false;
        @autoreleasepool {
            id parsed = [NSJSONSerialization JSONObjectWithData:[NSData dataWithBytes:bytes length:size] options:0 error:nil];
            if (![parsed isKindOfClass:NSDictionary.class]) return false;
            NSDictionary* request = parsed;
            if (![request[@"type"] isEqual:@"host-command"] || ![request[@"kind"] isEqual:@"restart-mac"]) return false;
            NSString* requestId = [request[@"request_id"] isKindOfClass:NSString.class] ? request[@"request_id"] : nil;
            NSUUID* uuid = requestId.length == 36 ? [[NSUUID alloc] initWithUUIDString:requestId] : nil;
            if (!uuid) return false;
            std::string key = uuid.UUIDString.UTF8String;
            NSData* reply = nil;
            bool start = false;
            {
                std::lock_guard lock(mutex);
                if (!connected) return false;
                auto duplicate = replies.find(key);
                if (duplicate != replies.end()) reply = duplicate->second;
                else {
                    NSSet* keys = [NSSet setWithArray:request.allKeys];
                    NSSet* allowed = [NSSet setWithArray:@[@"type",@"kind",@"request_id",@"confirmed"]];
                    id confirmation = request[@"confirmed"];
                    bool confirmed = confirmation && CFGetTypeID((__bridge CFTypeRef)confirmation) == CFBooleanGetTypeID() && [confirmation boolValue];
                    if (![keys isEqualToSet:allowed] || !confirmed) {
                        reply = Reply(requestId, {"error","confirmation_required","重启请求无效：请先在 Windows 端确认重启。"});
                    } else if (busy) {
                        reply = Reply(requestId, {"busy","restart_in_progress","已有重启请求正在等待 Mac 系统授权。"});
                    } else {
                        busy = true; cancelled.store(false); active = key; start = true;
                        reply = Reply(requestId, {"authorizing","local_authorization_required","请在 Mac 系统授权对话框中确认管理员权限；授权后将立即重启。"});
                    }
                    remember(key, reply);
                }
            }
            emit(reply);
            if (!start) return true;
            if (worker.joinable()) worker.join();
            remote_log::Write(remote_log::Level::Info, "restart", "authorization started");
            worker = std::thread([this, key, requestId] {
                @autoreleasepool {
                    Result result = run(cancelled);
                    NSData* final = Reply(requestId, result);
                    bool deliver;
                    {
                        std::lock_guard lock(mutex);
                        if (cancelled.load()) result = kCancelled;
                        final = Reply(requestId, result);
                        remember(key, final); active.clear(); busy = false; deliver = connected;
                    }
                    remote_log::Write(remote_log::Level::Info, "restart", result.code);
                    if (deliver) emit(final);
                }
            });
            return true;
        }
    }
};

RemoteMacRestart::RemoteMacRestart(Send send) : impl_(std::make_unique<Impl>(std::move(send), RunNativeAuthorization)) {}
RemoteMacRestart::~RemoteMacRestart() = default;
void RemoteMacRestart::SetConnected(bool connected) { impl_->connection(connected); }
bool RemoteMacRestart::Handle(const uint8_t* json, size_t size) { return impl_->handle(json, size); }
bool RemoteMacRestart::Busy() const { std::lock_guard lock(impl_->mutex); return impl_->busy; }

int RemoteMacRestart::SelfTest() {
    @autoreleasepool {
        NSMutableArray* checks = [NSMutableArray array];
        auto check = [&](const char* name, bool passed) { [checks addObject:@{@"name":[NSString stringWithUTF8String:name],@"passed":@(passed)}]; };
        std::mutex replyMutex; NSMutableArray* statuses = [NSMutableArray array];
        auto send = [&](const uint8_t* data, size_t size) {
            @autoreleasepool {
                id json = [NSJSONSerialization JSONObjectWithData:[NSData dataWithBytes:data length:size] options:0 error:nil];
                std::lock_guard lock(replyMutex); if (json) [statuses addObject:json];
            }
        };
        auto input = [](Impl& impl, NSDictionary* json) {
            NSData* data = [NSJSONSerialization dataWithJSONObject:json options:0 error:nil];
            return impl.handle((const uint8_t*)data.bytes, data.length);
        };
        NSString* requestId = @"7BB078DE-B177-4AAC-A76C-32E69B760280";
        NSDictionary* request = @{@"type":@"host-command",@"kind":@"restart-mac",@"request_id":requestId,@"confirmed":@YES};
        std::atomic<int> calls{0}; std::atomic<bool> release{false};
        Impl impl(send, [&](const std::atomic<bool>& cancel) {
            calls.fetch_add(1);
            while (!release.load() && !cancel.load()) std::this_thread::sleep_for(std::chrono::milliseconds(1));
            return cancel.load() ? kCancelled : kRequested;
        });
        check("unpaired command rejected", !input(impl, request) && calls.load() == 0);
        impl.connection(true);
        const uint8_t junk[] = {1,2,3};
        check("malformed JSON rejected", !impl.handle(junk, sizeof(junk)));
        std::vector<uint8_t> oversized(1025);
        check("oversized command rejected", !impl.handle(oversized.data(), oversized.size()));
        check("unknown command rejected", !input(impl, @{@"type":@"host-command",@"kind":@"shell",@"request_id":requestId,@"confirmed":@YES}));
        check("invalid request ID rejected", !input(impl, @{@"type":@"host-command",@"kind":@"restart-mac",@"request_id":@"bad",@"confirmed":@YES}));
        input(impl, @{@"type":@"host-command",@"kind":@"restart-mac",@"request_id":@"69B0C80E-8B09-4C71-BDB7-329F0004EDAF",@"confirmed":@1});
        check("confirmation must be Boolean", calls.load() == 0);
        input(impl, @{@"type":@"host-command",@"kind":@"restart-mac",@"request_id":@"283F7D01-B3FC-40B7-B00A-D1CE4F064B80",@"confirmed":@YES,@"command":@"anything"});
        check("extra command arguments rejected", calls.load() == 0);
        check("confirmed paired command accepted", input(impl, request));
        for (int i = 0; i < 100 && calls.load() != 1; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(1));
        input(impl, request); check("duplicate request runs only once", calls.load() == 1);
        input(impl, @{@"type":@"host-command",@"kind":@"restart-mac",@"request_id":@"5A2302EF-FA7B-40F5-ADFA-7D8E735A3FDC",@"confirmed":@YES});
        { std::lock_guard lock(replyMutex); check("concurrent restart reports busy", [[statuses.lastObject objectForKey:@"state"] isEqual:@"busy"] && calls.load() == 1); }
        release.store(true); if (impl.worker.joinable()) impl.worker.join();
        input(impl, request);
        { std::lock_guard lock(replyMutex); check("completed request remains idempotent", calls.load() == 1 && [[statuses.lastObject objectForKey:@"state"] isEqual:@"requested"]); }
        release.store(false);
        NSDictionary* cancelledRequest = @{@"type":@"host-command",@"kind":@"restart-mac",@"request_id":@"48EEB047-FED8-4819-8819-E33E92BF44B3",@"confirmed":@YES};
        input(impl, cancelledRequest); impl.connection(false);
        if (impl.worker.joinable()) impl.worker.join();
        impl.connection(true); input(impl, cancelledRequest);
        { std::lock_guard lock(replyMutex); check("disconnect cancels pending authorization", [[statuses.lastObject objectForKey:@"state"] isEqual:@"cancelled"]); }
        Impl denied(send, [](const std::atomic<bool>&) { return kDenied; }); denied.connection(true); input(denied, request);
        if (denied.worker.joinable()) denied.worker.join();
        { std::lock_guard lock(replyMutex); check("permission denial has explicit safe feedback", [[statuses.lastObject objectForKey:@"code"] isEqual:@"authorization_denied"]); }
        Impl timeout(send, [](const std::atomic<bool>&) { return kTimeout; }); timeout.connection(true); input(timeout, request);
        if (timeout.worker.joinable()) timeout.worker.join();
        { std::lock_guard lock(replyMutex); check("authorization timeout is reported", [[statuses.lastObject objectForKey:@"code"] isEqual:@"authorization_timeout"]); }
        check("native script accepts no credentials or caller parameters", !strstr(kRestartScript,"password") && !strstr(kRestartScript,"user name") && !strstr(kRestartScript,"sudo"));
        NSAppleScript* source = [[NSAppleScript alloc] initWithSource:[NSString stringWithUTF8String:kRestartScript]];
        check("fixed native authorization script compiles without execution", [source compileAndReturnError:nil]);
        const auto previousLanguage = remote_i18n::GetLanguage();
        remote_i18n::SetLanguage(remote_i18n::Language::English);
        NSAppleScript *englishSource = [[NSAppleScript alloc] initWithSource:[NSString stringWithUTF8String:RestartAuthorizationScript().c_str()]];
        check("English authorization prompt compiles without execution", [englishSource compileAndReturnError:nil]);
        remote_i18n::SetLanguage(remote_i18n::Language::SimplifiedChinese);
        NSAppleScript *chineseSource = [[NSAppleScript alloc] initWithSource:[NSString stringWithUTF8String:RestartAuthorizationScript().c_str()]];
        check("Simplified Chinese authorization prompt compiles without execution", [chineseSource compileAndReturnError:nil]);
        remote_i18n::SetLanguage(previousLanguage);
        bool passed = true; for (NSDictionary* value in checks) if (![value[@"passed"] boolValue]) passed = false;
        NSData* json = [NSJSONSerialization dataWithJSONObject:@{@"schema":@"remote.restart-self-test.v1",@"passed":@(passed),@"real_restart_executed":@NO,@"checks":checks} options:NSJSONWritingPrettyPrinted error:nil];
        fwrite(json.bytes, 1, json.length, stdout); fputc('\n', stdout);
        return passed ? 0 : 1;
    }
}
