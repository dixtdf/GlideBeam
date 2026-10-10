#pragma once
#include <string>

// Localize only fixed UI and status strings. User content and protocol values
// are deliberately left unchanged. Platform code owns preference persistence.
namespace remote_i18n {
enum class Language { System, English, SimplifiedChinese };
using SystemLanguageResolver = Language (*)();
void SetLanguage(Language language);
Language GetLanguage();
Language EffectiveLanguage();
void SetSystemLanguageResolver(SystemLanguageResolver resolver);
const char* LanguageTag(Language language);
Language ParseLanguageTag(const std::string& tag);
const char* Text(const char* source);
const wchar_t* Text(const wchar_t* source);
std::string Translate(const std::string& source);
std::wstring Translate(const std::wstring& source);
}
