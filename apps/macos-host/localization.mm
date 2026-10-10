#import "localization.h"
#include <cstdio>

NSString *const RemoteHostLanguageChangedNotification = @"dev.remote-native.host.language-changed";

namespace {
remote_i18n::Language SystemLanguage() {
    NSString *language = NSLocale.preferredLanguages.firstObject ?: @"en";
    // Traditional Chinese locales use the English fallback until a Traditional
    // Chinese catalog exists. Never silently render Simplified Chinese for them.
    if ([language hasPrefix:@"zh-Hans"] || [language hasPrefix:@"zh-CN"] ||
        [language hasPrefix:@"zh-SG"] || [language isEqualToString:@"zh"])
        return remote_i18n::Language::SimplifiedChinese;
    return remote_i18n::Language::English;
}
NSDictionary<NSString *, NSString *> *EnglishCatalog() {
    static NSDictionary<NSString *, NSString *> *catalog;
    static dispatch_once_t once;
    dispatch_once(&once, ^{
        NSString *resource = [NSBundle.mainBundle.resourcePath stringByAppendingPathComponent:@"en.lproj/Localizable.strings"];
        catalog = [NSDictionary dictionaryWithContentsOfFile:resource] ?: @{};
    });
    return catalog;
}
}

NSString *RemoteHostText(NSString *source) {
    if (!source) return @"";
    if (remote_i18n::EffectiveLanguage() == remote_i18n::Language::SimplifiedChinese) {
        // A brief copy notification may have been created before a language
        // change. Resolve its known English title back to the canonical key.
        NSString *key = [EnglishCatalog() allKeysForObject:source].firstObject;
        if (key) return key;
        return [NSString stringWithUTF8String:remote_i18n::Translate(std::string(source.UTF8String ?: "")).c_str()] ?: source;
    }
    NSString *value = EnglishCatalog()[source];
    if (value) return value;
    return [NSString stringWithUTF8String:remote_i18n::Translate(std::string(source.UTF8String ?: "")).c_str()] ?: source;
}
NSString *RemoteHostCanonicalText(NSString *source) {
    return [EnglishCatalog() allKeysForObject:source].firstObject ?: source;
}
NSString *RemoteHostBrand(void) {
    return remote_i18n::EffectiveLanguage() == remote_i18n::Language::SimplifiedChinese ? @"光梭" : @"GlideBeam";
}
NSString *RemoteHostLanguageName(remote_i18n::Language language) {
    if (language == remote_i18n::Language::English) return @"English";
    if (language == remote_i18n::Language::SimplifiedChinese) return @"简体中文";
    return RemoteHostText(@"跟随系统");
}
void RemoteHostLoadLanguagePreference(void) {
    remote_i18n::SetSystemLanguageResolver(SystemLanguage);
    NSString *tag = [NSUserDefaults.standardUserDefaults stringForKey:@"RemoteUILanguage"] ?: @"system";
    remote_i18n::SetLanguage(remote_i18n::ParseLanguageTag(tag.UTF8String));
}
void RemoteHostSetLanguagePreference(remote_i18n::Language language) {
    remote_i18n::SetLanguage(language);
    [NSUserDefaults.standardUserDefaults setObject:[NSString stringWithUTF8String:remote_i18n::LanguageTag(language)] forKey:@"RemoteUILanguage"];
    [NSNotificationCenter.defaultCenter postNotificationName:RemoteHostLanguageChangedNotification object:nil];
}
BOOL RemoteHostLocalizationSelfTest(void) {
    const auto previous = remote_i18n::GetLanguage();
    BOOL okay = EnglishCatalog().count >= 160;
    remote_i18n::SetLanguage(remote_i18n::Language::English);
    okay = okay && [RemoteHostText(@"复制令牌") isEqualToString:@"Copy token"] && [RemoteHostBrand() isEqualToString:@"GlideBeam"];
    NSString *user = @"user-file-🦋.txt";
    okay = okay && [RemoteHostText(user) isEqualToString:user];
    remote_i18n::SetLanguage(remote_i18n::Language::SimplifiedChinese);
    okay = okay && [RemoteHostText(@"复制令牌") isEqualToString:@"复制令牌"] && [RemoteHostBrand() isEqualToString:@"光梭"];
    remote_i18n::SetLanguage(previous);
    printf("{\"suite\":\"mac-localization\",\"passed\":%s,\"catalog_entries\":%lu,\"languages\":[\"system\",\"en\",\"zh-Hans\"]}\n",okay ? "true" : "false",(unsigned long)EnglishCatalog().count);
    return okay;
}
