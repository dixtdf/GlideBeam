#include "../../apps/windows-client/ui-watchdog.h"
#include "../diagnostic-log.h"
#include <windows.h>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>
#include <thread>

int main(int argc, char** argv) {
    if (argc != 2) return 2;
    std::filesystem::path directory = argv[1];
    remote_log::Initialize({directory});
    remote_ui_watchdog::Start(GetCurrentThreadId());
    remote_ui_watchdog::Beat("synthetic-delayed-clipboard");
    std::this_thread::sleep_for(std::chrono::milliseconds(6200));
    remote_ui_watchdog::Beat();
    std::this_thread::sleep_for(std::chrono::milliseconds(1200));
    const auto start = GetTickCount64();
    remote_ui_watchdog::Stop();
    const auto stopMs = GetTickCount64() - start;
    remote_log::Shutdown();
    std::ifstream file(directory / "remote-native.log");
    std::string log((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    bool pass = log.find("heartbeat_stalled") != std::string::npos &&
        log.find("synthetic-delayed-clipboard") != std::string::npos &&
        log.find("heartbeat_recovered") != std::string::npos && stopMs < 1000;
    std::cout << "{\"passed\":" << (pass ? "true" : "false")
        << ",\"synthetic_stall\":true,\"process_memory_read\":false,\"stop_ms\":" << stopMs << "}\n";
    return pass ? 0 : 1;
}
