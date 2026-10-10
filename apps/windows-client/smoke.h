#pragma once
#include "ui.h"
// Own native diagnostic window: receives video, never sends remote user input.
// Exit 0 requires successful hardware-decoded/presented frames and a saved report.
int RunConnectionSmoke(HINSTANCE instance, unsigned seconds, const wchar_t* reportPath, const std::string& addressOverride = {});
