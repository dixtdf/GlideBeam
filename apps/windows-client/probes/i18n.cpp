#include "../../../shared/i18n.h"
#include "../language.h"
#include <iostream>
#include <fstream>
#include <vector>
#include <utility>
using namespace remote_i18n;
Language ChineseSystem(){return Language::SimplifiedChinese;}
Language EnglishSystem(){return Language::English;}
int main(int argc,char** argv){
    std::vector<std::pair<std::string,bool>> checks;
    auto check=[&](const char* name,bool value){checks.emplace_back(name,value);};
    SetLanguage(Language::English);
    check("english_ui",Translate("配对令牌")=="Pairing token");
    check("english_common_error",Translate("连接超时，请检查 Mac 地址和网络连接").find("timed out")!=std::string::npos);
    check("english_capture_message",Translate("画面采集暂时中断，正在自动恢复…").find("Recovering automatically")!=std::string::npos);
    check("format_parameter_preserved",std::wstring(Text(L"%u 帧"))==L"%u frames");
    check("unknown_content_unchanged",Translate("自定义文件夹/未经翻译的用户内容") == "自定义文件夹/未经翻译的用户内容");
    const char* persistent=Text("配对令牌");SetLanguage(Language::SimplifiedChinese);
    check("chinese_reverse_catalog",Translate("Pairing token")=="配对令牌");
    check("catalog_pointers_stable",std::string(persistent)=="Pairing token");
    SetLanguage(Language::System);SetSystemLanguageResolver(ChineseSystem);
    check("follow_chinese_system",EffectiveLanguage()==Language::SimplifiedChinese&&Translate("Pairing token")=="配对令牌");
    SetSystemLanguageResolver(EnglishSystem);
    check("follow_english_system",EffectiveLanguage()==Language::English&&Translate("配对令牌")=="Pairing token");
    check("language_tags",ParseLanguageTag("zh-CN")==Language::SimplifiedChinese&&ParseLanguageTag("en")==Language::English&&ParseLanguageTag("unknown")==Language::System);
    check("null_text_safe",std::string(Text(static_cast<const char*>(nullptr))).empty());
    wchar_t existing[32768]{};DWORD length=GetEnvironmentVariableW(L"LOCALAPPDATA",existing,32768);
    auto directory=std::filesystem::temp_directory_path()/(L"GlideBeamLanguageProbe-"+std::to_wstring(GetCurrentProcessId()));
    SetEnvironmentVariableW(L"LOCALAPPDATA",directory.c_str());
    check("preference_save",remote_windows_language::Save(Language::English));SetLanguage(Language::SimplifiedChinese);remote_windows_language::Load();
    check("preference_reload",GetLanguage()==Language::English);
    check("connection_settings_untouched",!std::filesystem::exists(remote_windows_language::PreferencePath().parent_path()/L"connection.json"));
    SetEnvironmentVariableW(L"LOCALAPPDATA",length&&length<32768?existing:nullptr);
    std::error_code error;std::filesystem::remove_all(directory,error);
    bool passed=true;for(auto& item:checks)passed&=item.second;
    std::ostream* output=&std::cout;std::ofstream report;if(argc>1){report.open(argv[1]);output=&report;}
    *output<<"{\n  \"synthetic_only\": true,\n  \"checks\": {\n";
    for(size_t i=0;i<checks.size();++i)*output<<"    \""<<checks[i].first<<"\": "<<(checks[i].second?"true":"false")<<(i+1<checks.size()?",":"")<<'\n';
    *output<<"  },\n  \"passed\": "<<(passed?"true":"false")<<"\n}\n";
    if(argc>1)std::cout<<"Language checks: "<<(passed?"PASS":"FAIL")<<" ("<<checks.size()<<")\n";
    return passed?0:1;
}
