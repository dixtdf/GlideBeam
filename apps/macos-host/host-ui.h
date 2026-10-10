#pragma once
#import <AppKit/AppKit.h>
#include <stdint.h>

// Display-only state. Authentication, capture and transport remain in RemoteHost.
@interface RemoteHostPanelState : NSObject
@property(copy) NSString *address;
@property(copy) NSString *token;
@property(copy) NSString *fingerprint;
@property(copy) NSString *status;
@property BOOL screenGranted;
@property BOOL inputGranted;
@property BOOL capturing;
@property BOOL starting;
@property BOOL connected;
@property BOOL cursorBaked;
@property BOOL preview;
@property uint32_t width;
@property uint32_t height;
@property uint64_t encoded;
@property uint64_t replaced;
@property NSUInteger blockedCount;
@property BOOL loginStartup;
@property BOOL exchangeEnabled;
@property(copy) NSString *exchangeStatus;
@end

@interface RemoteHostPanel : NSView
- (instancetype)initWithTarget:(id)target permissions:(SEL)permissions retry:(SEL)retry
                     editToken:(SEL)editToken blacklist:(SEL)blacklist loginStartup:(SEL)loginStartup;
- (void)updateState:(RemoteHostPanelState *)state;
- (void)setExchangeTarget:(id)target toggle:(SEL)toggle files:(SEL)files;
- (void)applyLocalization;
@end

NSString *RemoteHostLocalizedStatus(NSString *message);
NSString *RemoteHostConnectionAddress(NSString *bind);
void RemoteHostInstallApplicationMenu(void);
NSMenu *RemoteHostMakeLanguageMenu(id target, SEL action);
BOOL RemoteHostRenderPreview(NSString *path);
NSButton *RemoteHostMakeButton(NSString *title, id target, SEL action, BOOL primary);
