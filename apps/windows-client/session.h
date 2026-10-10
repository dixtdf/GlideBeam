#pragma once
#include "ui.h"
#include <memory>
std::unique_ptr<IClientSession> CreateClientSession();
ConnectionOptions LoadConnectionOptions();
bool SaveConnectionOptions(const ConnectionOptions& options);
bool ImportPairingFile(const wchar_t* path);
bool RunSessionSelfTest();
