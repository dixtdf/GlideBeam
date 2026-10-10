#import <AppKit/AppKit.h>
#import <ApplicationServices/ApplicationServices.h>
#import <Foundation/Foundation.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>

static NSString *const ExpectedText = @"远控-RemoteNative-testa";
static bool WriteJson(NSString *path, NSDictionary *value) {
    if (!path.length) return false;
    NSData *data = [NSJSONSerialization dataWithJSONObject:value options:NSJSONWritingSortedKeys error:nil];
    return data && [data writeToFile:path options:NSDataWritingAtomic error:nil];
}

@interface InputPatternDelegate : NSObject <NSApplicationDelegate, NSWindowDelegate>
@property(nonatomic, strong) NSWindow *window;
@property(nonatomic, strong) NSTextField *field;
@property(nonatomic, strong) NSTextField *status;
@property(nonatomic, strong) NSTimer *timer;
@property(nonatomic, strong) id monitor;
@property(nonatomic, strong) id activationObserver;
@property(nonatomic, copy) NSString *readyPath;
@property(nonatomic, copy) NSString *reportPath;
@property(nonatomic, copy) NSString *nonce;
@property(nonatomic, strong) NSDictionary *lastReady;
@property(nonatomic) NSTimeInterval duration;
@property(nonatomic) NSTimeInterval started;
@property(nonatomic) NSUInteger mouseDownCount;
@property(nonatomic) NSUInteger mouseUpCount;
@property(nonatomic) NSUInteger scrollCount;
@property(nonatomic) NSUInteger keyDownCount;
@property(nonatomic) NSUInteger keyUpCount;
@property(nonatomic) NSUInteger flagsChangedCount;
@property(nonatomic) BOOL shiftObservedDown;
@property(nonatomic) BOOL shiftCurrentlyDown;
@property(nonatomic) BOOL completed;
@property(nonatomic) BOOL reported;
@property(nonatomic) BOOL everFrontmost;
@property(nonatomic) BOOL activationCancelled;
@property(nonatomic) BOOL activationApiAccepted;
@property(nonatomic) NSUInteger activationAttempts;
- (void)updateReady;
- (void)requestOwnActivation;
- (void)finish;
@end

@implementation InputPatternDelegate
- (BOOL)isOwnAppFrontmost {
    return NSWorkspace.sharedWorkspace.frontmostApplication.processIdentifier == NSProcessInfo.processInfo.processIdentifier;
}
- (void)requestOwnActivation {
    if (self.reported || self.everFrontmost || self.activationCancelled || self.activationAttempts >= 4) return;
    ++self.activationAttempts;
    // Only request activation for this fixture. A successful request is not proof
    // of frontmost ownership: all input safety checks still use NSWorkspace's PID.
    if (@available(macOS 14.0, *)) [NSApp activate];
    else [NSApp activateIgnoringOtherApps:YES];
    self.activationApiAccepted = [NSRunningApplication.currentApplication activateWithOptions:NSApplicationActivateAllWindows];
    [self.window makeKeyAndOrderFront:nil];
    [self.window makeFirstResponder:self.field];
    [self updateReady];
}
- (NSDictionary *)identity {
    NSRunningApplication *app = NSRunningApplication.currentApplication;
    return @{ @"pid":@(NSProcessInfo.processInfo.processIdentifier),
              @"target_name":@"InputPattern",
              @"target_bundle_id":app.bundleIdentifier ?: @"",
              @"target_executable":NSBundle.mainBundle.executablePath ?: @"",
              @"session_nonce":self.nonce ?: @"" };
}
- (NSDictionary *)counts {
    [self.field validateEditing];
    return @{ @"mouse_down":@(self.mouseDownCount), @"mouse_up":@(self.mouseUpCount),
              @"scroll":@(self.scrollCount), @"key_down":@(self.keyDownCount), @"key_up":@(self.keyUpCount),
              @"flags_changed":@(self.flagsChangedCount), @"shift_observed_down":@(self.shiftObservedDown),
              @"shift_currently_down":@(self.shiftCurrentlyDown),
              @"matches_expected_text":@([self.field.stringValue isEqualToString:ExpectedText]),
              @"text_length":@(self.field.stringValue.length) };
}
- (void)applicationDidFinishLaunching:(NSNotification *)notification {
    (void)notification;
    self.nonce = NSUUID.UUID.UUIDString;
    self.started = NSProcessInfo.processInfo.systemUptime;
    self.window = [[NSWindow alloc] initWithContentRect:NSMakeRect(0,0,690,300)
        styleMask:NSWindowStyleMaskTitled|NSWindowStyleMaskClosable backing:NSBackingStoreBuffered defer:NO];
    self.window.title = @"Remote Native 输入验证";
    self.window.delegate = self; self.window.releasedWhenClosed = NO;
    self.window.backgroundColor = [NSColor colorWithSRGBRed:.055 green:.08 blue:.12 alpha:1];
    NSTextField *title = [NSTextField labelWithString:@"Remote Native 输入验证"];
    title.frame = NSMakeRect(28,236,620,32); title.font = [NSFont systemFontOfSize:22 weight:NSFontWeightSemibold];
    title.textColor = NSColor.whiteColor; [self.window.contentView addSubview:title];
    NSTextField *hint = [NSTextField wrappingLabelWithString:@"此窗口只验证固定文案、点击、滚轮和按键。报告只保存计数和匹配结果。"];
    hint.frame = NSMakeRect(28,184,630,38); hint.textColor = NSColor.secondaryLabelColor;
    [self.window.contentView addSubview:hint];
    self.field = [[NSTextField alloc] initWithFrame:NSMakeRect(95,122,500,40)];
    self.field.stringValue = @""; self.field.placeholderString = ExpectedText;
    self.field.font = [NSFont systemFontOfSize:19]; self.field.focusRingType = NSFocusRingTypeDefault;
    [self.window.contentView addSubview:self.field];
    self.status = [NSTextField wrappingLabelWithString:@"等待通过远端通道发送固定测试输入…"];
    self.status.frame = NSMakeRect(28,32,630,67); self.status.textColor = NSColor.secondaryLabelColor;
    [self.window.contentView addSubview:self.status];
    NSMenu *menu = [NSMenu new]; NSMenuItem *applicationItem = [NSMenuItem new];
    NSMenu *applicationMenu = [[NSMenu alloc] initWithTitle:@"InputPattern"];
    [applicationMenu addItemWithTitle:@"退出输入验证" action:@selector(terminate:) keyEquivalent:@"q"];
    applicationItem.submenu = applicationMenu; [menu addItem:applicationItem]; NSApp.mainMenu = menu;
    [self.window center]; [self.window makeKeyAndOrderFront:nil];
    [self.window makeFirstResponder:self.field];
    __weak InputPatternDelegate *weakSelf = self;
    NSEventMask mask = NSEventMaskLeftMouseDown|NSEventMaskLeftMouseUp|NSEventMaskRightMouseDown|NSEventMaskRightMouseUp|
        NSEventMaskOtherMouseDown|NSEventMaskOtherMouseUp|NSEventMaskScrollWheel|NSEventMaskKeyDown|NSEventMaskKeyUp|NSEventMaskFlagsChanged;
    self.monitor = [NSEvent addLocalMonitorForEventsMatchingMask:mask handler:^NSEvent *(NSEvent *event) {
        InputPatternDelegate *owner = weakSelf;
        if (!owner || event.window != owner.window) return event;
        switch (event.type) {
            case NSEventTypeLeftMouseDown: case NSEventTypeRightMouseDown: case NSEventTypeOtherMouseDown: ++owner.mouseDownCount; break;
            case NSEventTypeLeftMouseUp: case NSEventTypeRightMouseUp: case NSEventTypeOtherMouseUp: ++owner.mouseUpCount; break;
            case NSEventTypeScrollWheel: ++owner.scrollCount; break;
            case NSEventTypeKeyDown: ++owner.keyDownCount; break;
            case NSEventTypeKeyUp: ++owner.keyUpCount; break;
            case NSEventTypeFlagsChanged:
                ++owner.flagsChangedCount;
                owner.shiftCurrentlyDown = !!(event.modifierFlags & NSEventModifierFlagShift);
                if (owner.shiftCurrentlyDown) owner.shiftObservedDown = YES;
                break;
            default: break;
        }
        // Do not inspect characters, key codes, clipboard, or another application's events.
        return event;
    }];
    self.activationObserver = [NSWorkspace.sharedWorkspace.notificationCenter addObserverForName:NSWorkspaceDidActivateApplicationNotification
        object:nil queue:NSOperationQueue.mainQueue usingBlock:^(NSNotification *note) {
            InputPatternDelegate *owner = weakSelf; if (!owner) return;
            NSRunningApplication *activated = note.userInfo[NSWorkspaceApplicationKey];
            if (owner.activationAttempts && activated && activated.processIdentifier != NSProcessInfo.processInfo.processIdentifier)
                owner.activationCancelled = YES;
            [owner updateReady];
        }];
    self.timer = [NSTimer timerWithTimeInterval:.02 repeats:YES block:^(NSTimer *timer) {
        (void)timer; InputPatternDelegate *owner = weakSelf; if (!owner) return;
        [owner updateReady];
        double elapsed = NSProcessInfo.processInfo.systemUptime-owner.started;
        if (elapsed < 2 && elapsed >= .15 + owner.activationAttempts*.25) [owner requestOwnActivation];
        if (NSProcessInfo.processInfo.systemUptime-owner.started >= owner.duration) { owner.completed=YES; [owner finish]; }
    }];
    [NSRunLoop.mainRunLoop addTimer:self.timer forMode:NSRunLoopCommonModes];
    [self updateReady];
    // Let applicationDidFinishLaunching return before the activation request.
    dispatch_async(dispatch_get_main_queue(), ^{ [weakSelf requestOwnActivation]; });
}
- (void)updateReady {
    if (self.reported || !self.window) return;
    NSScreen *mainScreen = nil;
    for (NSScreen *screen in NSScreen.screens)
        if ([screen.deviceDescription[@"NSScreenNumber"] unsignedIntValue] == CGMainDisplayID()) mainScreen = screen;
    if (!mainScreen) mainScreen = NSScreen.mainScreen;
    NSRect fieldWindow = [self.field convertRect:self.field.bounds toView:nil];
    NSRect fieldScreen = [self.window convertRectToScreen:fieldWindow];
    NSRect nsBounds = mainScreen.frame;
    CGRect cgBounds = CGDisplayBounds(CGMainDisplayID());
    double cgX = cgBounds.origin.x + (NSMidX(fieldScreen)-nsBounds.origin.x)*cgBounds.size.width/nsBounds.size.width;
    double cgY = cgBounds.origin.y + (NSMaxY(nsBounds)-NSMidY(fieldScreen))*cgBounds.size.height/nsBounds.size.height;
    double x = (cgX-cgBounds.origin.x)/std::max(1.0,cgBounds.size.width-1);
    double y = (cgY-cgBounds.origin.y)/std::max(1.0,cgBounds.size.height-1);
    BOOL valid = std::isfinite(x)&&std::isfinite(y)&&x>=0&&x<=1&&y>=0&&y<=1;
    BOOL frontmost = [self isOwnAppFrontmost];
    if (frontmost) self.everFrontmost = YES;
    NSRunningApplication *ownApp = NSRunningApplication.currentApplication;
    NSRunningApplication *frontApp = NSWorkspace.sharedWorkspace.frontmostApplication;
    NSMutableDictionary *ready = [[self identity] mutableCopy];
    [ready addEntriesFromDictionary:[self counts]];
    [ready addEntriesFromDictionary:@{ @"schema":@"remote.input-pattern.ready.v1", @"ready":@(self.window.visible&&!self.window.miniaturized&&valid),
        @"own_app_frontmost":@(frontmost), @"normalized_x":@(valid?x:0), @"normalized_y":@(valid?y:0),
        @"application_active":@(NSApp.active), @"own_running_app_active":@(ownApp.active),
        @"own_running_app_finished_launching":@(ownApp.finishedLaunching), @"activation_policy":@(NSApp.activationPolicy),
        @"activation_attempts":@(self.activationAttempts), @"activation_api_accepted":@(self.activationApiAccepted),
        @"ever_frontmost":@(self.everFrontmost), @"startup_activation_cancelled":@(self.activationCancelled),
        @"frontmost_pid":@(frontApp ? frontApp.processIdentifier : 0),
        @"expected_text":ExpectedText, @"window_visible":@(self.window.visible), @"coordinate_valid":@(valid),
        @"field_focused":@(self.field.currentEditor!=nil), @"captures_other_apps":@NO, @"global_input_monitor":@NO }];
    if (![ready isEqualToDictionary:self.lastReady] && WriteJson(self.readyPath,ready)) self.lastReady=ready;
    self.status.stringValue = [NSString stringWithFormat:@"剩余 %.0f 秒 · 固定文案匹配 %@\n点击 %lu/%lu · 滚轮 %lu · 按键 %lu/%lu · Shift 已释放 %@",
        std::max(0.0,self.duration-(NSProcessInfo.processInfo.systemUptime-self.started)),
        [self.field.stringValue isEqualToString:ExpectedText]?@"通过":@"等待", (unsigned long)self.mouseDownCount,(unsigned long)self.mouseUpCount,
        (unsigned long)self.scrollCount,(unsigned long)self.keyDownCount,(unsigned long)self.keyUpCount,self.shiftCurrentlyDown?@"否":@"是"];
}
- (void)finish {
    if (self.reported) return; self.reported=YES;
    [self.timer invalidate]; if (self.monitor) { [NSEvent removeMonitor:self.monitor]; self.monitor=nil; }
    if (self.activationObserver) { [NSWorkspace.sharedWorkspace.notificationCenter removeObserver:self.activationObserver]; self.activationObserver=nil; }
    NSMutableDictionary *report = [[self identity] mutableCopy]; [report addEntriesFromDictionary:[self counts]];
    [report addEntriesFromDictionary:@{ @"schema":@"remote.input-pattern.report.v1", @"completed":@(self.completed),
        @"elapsed_seconds":@(NSProcessInfo.processInfo.systemUptime-self.started), @"requested_seconds":@(self.duration),
        @"own_app_frontmost":@([self isOwnAppFrontmost]), @"expected_text":ExpectedText,
        @"captures_other_apps":@NO, @"global_input_monitor":@NO, @"logs_free_text":@NO, @"injects_input":@NO }];
    WriteJson(self.reportPath,report);
    NSMutableDictionary *notReady = [report mutableCopy]; notReady[@"schema"]=@"remote.input-pattern.ready.v1"; notReady[@"ready"]=@NO;
    WriteJson(self.readyPath,notReady);
    NSData *json = [NSJSONSerialization dataWithJSONObject:report options:NSJSONWritingSortedKeys error:nil];
    if (json) { fwrite(json.bytes,1,json.length,stdout); fputc('\n',stdout); fflush(stdout); }
    [self.window orderOut:nil]; [NSApp terminate:nil];
}
- (void)windowWillClose:(NSNotification *)notification { (void)notification; [self finish]; }
@end

int main(int argc,const char *argv[]) {
    @autoreleasepool {
        NSString *ready=nil,*report=nil; double seconds=30;
        for (int i=1;i<argc;++i) {
            NSString *arg=[NSString stringWithUTF8String:argv[i]];
            if ([arg isEqualToString:@"--ready"] && i+1<argc) ready=[NSString stringWithUTF8String:argv[++i]];
            else if ([arg isEqualToString:@"--report"] && i+1<argc) report=[NSString stringWithUTF8String:argv[++i]];
            else if ([arg isEqualToString:@"--seconds"] && i+1<argc) { char *end=nullptr; seconds=strtod(argv[++i],&end); if (!end||*end||!std::isfinite(seconds)||seconds<1||seconds>120) return 2; }
            else return 2;
        }
        if (!ready.isAbsolutePath || !report.isAbsolutePath || [ready isEqualToString:report]) return 2;
        [NSApplication sharedApplication]; [NSApp setActivationPolicy:NSApplicationActivationPolicyRegular];
        InputPatternDelegate *delegate=[InputPatternDelegate new]; delegate.readyPath=ready; delegate.reportPath=report; delegate.duration=seconds;
        NSApp.delegate=delegate; [NSApp run]; return delegate.completed?0:1;
    }
}
