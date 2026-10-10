#pragma once
#include "ui.h"
#include "../../shared/exchange.h"
#include <memory>
#include <utility>
#include <functional>
class WindowsExchange {
public:
    WindowsExchange();
    ~WindowsExchange();
    void Attach(HWND owner, HWND canvas, IClientSession* session);
    void Tick();
    void ClipboardChanged();
    // Returns {kind: 1 clipboard, 2 files, transaction id}. User-initiated only.
    std::pair<int, uint64_t> PreparePaste();
    bool PastePending() const;
    std::pair<int, uint64_t> TakePreparedPaste();
    void CancelPendingPaste();
    void RemoteCopy();
    void ToggleClipboard();
    bool ClipboardEnabled() const;
    void ShowFiles();
    // Refresh labels without recreating the panel or interrupting file tasks.
    void RefreshLanguage();
    void Close();
private:
    friend bool RenderExchangePreview(const wchar_t* path);
    friend bool RunExchangeNativeSelfTest(const wchar_t* reportPath);
    friend void ProbeExchangeResponsiveness(const std::function<void(const char*, bool)>& check);
    struct Impl; std::unique_ptr<Impl> impl_;
};
bool RunExchangeNativeSelfTest(const wchar_t* reportPath);
bool RunExchangeHangSelfTest(const wchar_t* reportPath);
bool RenderExchangePreview(const wchar_t* path);
