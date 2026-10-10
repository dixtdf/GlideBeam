#pragma once
#import <Foundation/Foundation.h>
#include "../../shared/i18n.h"

// Translate known application labels only. User content, paths and tokens are
// passed through untouched when they are not catalog keys.
NSString *RemoteHostText(NSString *source);
NSString *RemoteHostBrand(void);
NSString *RemoteHostLanguageName(remote_i18n::Language language);
NSString *RemoteHostCanonicalText(NSString *source);
void RemoteHostLoadLanguagePreference(void);
void RemoteHostSetLanguagePreference(remote_i18n::Language language);
BOOL RemoteHostLocalizationSelfTest(void);
extern NSString *const RemoteHostLanguageChangedNotification;
