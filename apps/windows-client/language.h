#pragma once
#include "../../shared/i18n.h"
#include <windows.h>
#include <filesystem>
#include <fstream>
namespace remote_windows_language {
inline std::filesystem::path PreferencePath() {
    wchar_t path[32768]{};
    DWORD length=GetEnvironmentVariableW(L"LOCALAPPDATA",path,32768);
    return length&&length<32768?std::filesystem::path(path)/L"RemoteNative"/L"ui-language.txt":std::filesystem::path{};
}
inline void Load() {
    const auto path=PreferencePath();if(path.empty())return;
    std::ifstream file(path,std::ios::binary);std::string tag;
    if(file&&std::getline(file,tag)&&tag.size()<32)remote_i18n::SetLanguage(remote_i18n::ParseLanguageTag(tag));
}
inline bool Save(remote_i18n::Language language) {
    const auto path=PreferencePath();if(path.empty())return false;
    std::error_code error;std::filesystem::create_directories(path.parent_path(),error);if(error)return false;
    auto temporary=path;temporary+=L".tmp-"+std::to_wstring(GetCurrentProcessId());
    {std::ofstream file(temporary,std::ios::binary|std::ios::trunc);file<<remote_i18n::LanguageTag(language)<<'\n';file.flush();if(!file)return false;}
    const bool ok=MoveFileExW(temporary.c_str(),path.c_str(),MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH)!=FALSE;
    if(!ok)std::filesystem::remove(temporary,error);
    return ok;
}
}
