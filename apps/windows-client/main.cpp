#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif
#include "ui.h"
#include "language.h"
#include "session.h"
#include "smoke.h"
#include "decode-smoke.h"
#include "updater.h"
#include "exchange-windows.h"
#include "../../shared/diagnostic-log.h"
#include <shellapi.h>
#include <shobjidl.h>
#include <memory>
#include <string>

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int showCommand) {
    remote_windows_language::Load();
    remote_log::Initialize();
    struct DiagnosticsExit {
        ~DiagnosticsExit() {
            remote_log::Write(remote_log::Level::Info,"windows-app","shutdown complete");
            remote_log::Shutdown();
        }
    } diagnosticsExit;
    remote_log::Write(remote_log::Level::Info,"windows-app","startup");
    if (FAILED(SetCurrentProcessExplicitAppUserModelID(kGlideBeamAppUserModelId)))
        remote_log::Write(remote_log::Level::Warning,"windows-app","explicit AppUserModelID could not be set");
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    int argc = 0;
    auto argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    bool autoConnect = false;
    // This explicit flag is also used by deterministic CI preview tests.
    for (int i=1;i+1<argc;++i) if(std::wstring(argv[i])==L"--language") {
        std::wstring tag(argv[i+1]);std::string ascii(tag.begin(),tag.end());
        remote_i18n::SetLanguage(remote_i18n::ParseLanguageTag(ascii));
    }
    for (int i = 1; i < argc; ++i) {
        if(std::wstring(argv[i])==L"--preview-files"){if(i+1>=argc){LocalFree(argv);return 5;}bool ok=RenderExchangePreview(argv[i+1]);LocalFree(argv);return ok?0:2;}
        if(std::wstring(argv[i])==L"--selftest-exchange"){if(i+1>=argc){LocalFree(argv);return 5;}bool ok=RunExchangeNativeSelfTest(argv[i+1]);LocalFree(argv);return ok?0:3;}
        if(std::wstring(argv[i])==L"--selftest-exchange-hang"){if(i+1>=argc){LocalFree(argv);return 5;}bool ok=RunExchangeHangSelfTest(argv[i+1]);LocalFree(argv);return ok?0:3;}
        if (std::wstring(argv[i]) == L"--selftest-ui" || std::wstring(argv[i]) == L"--self-test-localization") {
            if (i + 1 >= argc) { LocalFree(argv); return 5; }
            bool passed = RunUiSelfTest(instance, argv[i + 1]);
            LocalFree(argv);
            return passed ? 0 : 3;
        }
        if (std::wstring(argv[i]) == L"--smoke-at") {
            if(i+3>=argc){LocalFree(argv);return 5;}
            std::wstring wide(argv[i+1]);std::string address;
            for(auto c:wide){if(c>127){LocalFree(argv);return 5;}address+=(char)c;}
            wchar_t* end=nullptr;unsigned long seconds=std::wcstoul(argv[i+2],&end,10);
            if(!end||*end||seconds<1||seconds>120){LocalFree(argv);return 5;}
            int result=RunConnectionSmoke(instance,(unsigned)seconds,argv[i+3],address);LocalFree(argv);return result;
        }
        if (std::wstring(argv[i]) == L"--decode-smoke") {
            if (i + 2 >= argc) { LocalFree(argv); return 5; }
            int result = RunDecodeSmoke(instance, argv[i + 1], argv[i + 2]);
            LocalFree(argv);
            return result;
        }
        if (std::wstring(argv[i]) == L"--smoke") {
            if (i + 2 >= argc) { LocalFree(argv); return 5; }
            wchar_t* end = nullptr;
            unsigned long seconds = std::wcstoul(argv[i + 1], &end, 10);
            if (!end || *end || seconds < 1 || seconds > 120) { LocalFree(argv); return 5; }
            int result = RunConnectionSmoke(instance, static_cast<unsigned>(seconds), argv[i + 2]);
            LocalFree(argv);
            return result;
        }
        std::wstring argument(argv[i]);
        if (argument == L"--preview" || argument == L"--preview-connected" || argument == L"--preview-fullscreen" ||
            argument == L"--preview-settings" || argument == L"--preview-connected-hidden" || argument == L"--preview-fullscreen-hidden") {
            if (i + 1 >= argc) { LocalFree(argv); return 5; }
            UiPreview mode = UiPreview::Landing;
            if (argument == L"--preview-settings") mode = UiPreview::Preferences;
            else if (argument == L"--preview-connected") mode = UiPreview::Connected;
            else if (argument == L"--preview-fullscreen") mode = UiPreview::Fullscreen;
            else if (argument == L"--preview-connected-hidden") mode = UiPreview::ConnectedHidden;
            else if (argument == L"--preview-fullscreen-hidden") mode = UiPreview::FullscreenHidden;
            bool rendered = RenderUiPreview(argv[++i], mode);
            LocalFree(argv);
            return rendered ? 0 : 2;
        }
        if (std::wstring(argv[i]) == L"--self-test") {
            LocalFree(argv);
            return RunSessionSelfTest() ? 0 : 3;
        }
        if (std::wstring(argv[i]) == L"--selftest-updater") {
            if (i + 1 >= argc) { LocalFree(argv); return 5; }
            const bool passed = remote_update::RunUpdaterSelfTest(argv[i + 1]);
            LocalFree(argv);
            return passed ? 0 : 3;
        }
        if (std::wstring(argv[i]) == L"--import-pairing" && i + 1 < argc) {
            bool imported = ImportPairingFile(argv[++i]);
            LocalFree(argv);
            return imported ? 0 : 4;
        }
        if (std::wstring(argv[i]) == L"--connect") autoConnect = true;
    }
    LocalFree(argv);
    struct InstanceGuard {
        HANDLE handle = nullptr;
        ~InstanceGuard() { if (handle) CloseHandle(handle); }
    } instanceGuard;
    instanceGuard.handle = CreateMutexW(nullptr, TRUE, L"Local\\dixtdf.GlideBeam.SingleInstance");
    const DWORD instanceStatus = GetLastError();
    if (!instanceGuard.handle) {
        remote_log::Write(remote_log::Level::Error,"windows-app","single-instance mutex creation failed");
        MessageBoxW(nullptr, remote_i18n::Text(L"光梭单实例初始化失败。"), remote_i18n::Text(L"光梭 · GlideBeam"), MB_OK | MB_ICONERROR);
        return 1;
    }
    if (instanceStatus == ERROR_ALREADY_EXISTS) {
        bool startReplacement = false;
        for (unsigned attempt = 0; attempt < 50; ++attempt) {
            HWND existing = FindWindowW(kGlideBeamWindowClass, nullptr);
            if (existing && PostMessageW(existing, kGlideBeamShowMessage, autoConnect ? 1 : 0, 0)) return 0;
            const DWORD wait = WaitForSingleObject(instanceGuard.handle, 0);
            if (wait == WAIT_OBJECT_0 || wait == WAIT_ABANDONED) { startReplacement = true; break; }
            if (wait == WAIT_FAILED) {
                remote_log::Write(remote_log::Level::Error,"windows-app","single-instance ownership check failed");
                return 1;
            }
            Sleep(100);
        }
        if (!startReplacement) {
            remote_log::Write(remote_log::Level::Warning,"windows-app","existing process has no main window; activation request not delivered");
            return 0;
        }
    }
    auto session = CreateClientSession();
    if (!session) {
        remote_log::Write(remote_log::Level::Error,"windows-app","session service initialization failed");
        MessageBoxW(nullptr, remote_i18n::Text(L"连接服务无法启动。"), remote_i18n::Text(L"光梭 · GlideBeam"), MB_OK | MB_ICONERROR);
        return 1;
    }
    remote_log::Write(remote_log::Level::Info,"windows-app","main UI started");
    int result=CreateUi(instance, showCommand, *session, LoadConnectionOptions(), autoConnect);
    remote_log::Write(remote_log::Level::Info,"windows-app","main UI exited code="+std::to_string(result));
    return result;
}
