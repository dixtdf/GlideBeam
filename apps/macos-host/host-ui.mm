#import "host-ui.h"
#import "localization.h"
#include "../../shared/diagnostic-log.h"
#include <arpa/inet.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netinet/in.h>
#include <cstring>
#include <initializer_list>

namespace {
constexpr unsigned Background = 0x0b1118, Panel = 0x121c27, Line = 0x24313f;
constexpr unsigned Foreground = 0xebf2fa, Muted = 0x8b9aad, Accent = 0x68e7c1;
constexpr unsigned Field = 0x0d151e, Soft = 0x173d38, Warning = 0xefc681;

NSColor *Color(unsigned rgb) {
    return [NSColor colorWithSRGBRed:((rgb >> 16) & 255) / 255.0
                             green:((rgb >> 8) & 255) / 255.0
                              blue:(rgb & 255) / 255.0 alpha:1];
}
void Rounded(NSRect rect, CGFloat radius, unsigned fill, unsigned border = 0) {
    NSBezierPath *path = [NSBezierPath bezierPathWithRoundedRect:NSInsetRect(rect, 0.5, 0.5)
                                                     xRadius:radius yRadius:radius];
    [Color(fill) setFill]; [path fill];
    if (border) { [Color(border) setStroke]; path.lineWidth = 1; [path stroke]; }
}
void Text(NSString *value, NSRect rect, CGFloat size, unsigned color,
          NSFontWeight weight = NSFontWeightRegular,
          NSTextAlignment alignment = NSTextAlignmentLeft) {
    NSMutableParagraphStyle *paragraph = [NSMutableParagraphStyle new];
    paragraph.alignment = alignment;
    paragraph.lineBreakMode = NSLineBreakByWordWrapping;
    NSDictionary *attributes = @{NSFontAttributeName:[NSFont systemFontOfSize:size weight:weight],
        NSForegroundColorAttributeName:Color(color), NSParagraphStyleAttributeName:paragraph};
    [value drawInRect:rect withAttributes:attributes];
}
void Dot(CGFloat x, CGFloat y, unsigned color) {
    [Color(color) setFill];
    [[NSBezierPath bezierPathWithOvalInRect:NSMakeRect(x, y, 7, 7)] fill];
}
} // namespace

@implementation RemoteHostPanelState
- (instancetype)init {
    if ((self = [super init])) {
        _address = @""; _token = @""; _fingerprint = @""; _status = RemoteHostText(@"正在启动…");
    }
    return self;
}
@end

@interface RemoteFlatButton : NSButton
@property BOOL primary;
@end
@implementation RemoteFlatButton
- (void)drawRect:(NSRect)dirtyRect {
    BOOL pressed = self.cell.isHighlighted;
    unsigned fill = self.primary ? (pressed ? 0x8df1d3 : Accent) : (pressed ? 0x263440 : 0x192633);
    Rounded(self.bounds, 7, fill, self.primary ? 0 : Line);
    unsigned color = self.enabled ? (self.primary ? 0x092d26 : Foreground) : Muted;
    CGFloat y = (self.bounds.size.height - 18) / 2 - 1;
    Text(self.title, NSMakeRect(6, y, self.bounds.size.width - 12, 22), 13, color,
         NSFontWeightMedium, NSTextAlignmentCenter);
    if (self.window.firstResponder == self) {
        NSBezierPath *focus = [NSBezierPath bezierPathWithRoundedRect:NSInsetRect(self.bounds, 3, 3)
                                                           xRadius:5 yRadius:5];
        [Color(self.primary ? 0x092d26 : Accent) setStroke]; focus.lineWidth = 1; [focus stroke];
    }
}
- (void)resetCursorRects {
    if (self.enabled) [self addCursorRect:self.bounds cursor:NSCursor.pointingHandCursor];
}
@end

@interface RemoteFlatCheckbox : NSButton
@end
@implementation RemoteFlatCheckbox
- (BOOL)isFlipped { return YES; }
- (void)drawRect:(NSRect)dirtyRect {
    CGFloat y=(self.bounds.size.height-16)/2;
    BOOL checked=self.state==NSControlStateValueOn;
    Rounded(NSMakeRect(1,y,16,16),4,checked ? Accent : Field,checked ? Accent : Line);
    if(checked) {
        NSBezierPath *mark=[NSBezierPath bezierPath];
        [mark moveToPoint:NSMakePoint(4,y+8)]; [mark lineToPoint:NSMakePoint(7.5,y+11.5)]; [mark lineToPoint:NSMakePoint(13.5,y+4.5)];
        [Color(Background) setStroke]; mark.lineWidth=1.8; [mark stroke];
    }
    Text(self.title,NSMakeRect(25,3,self.bounds.size.width-27,22),13,self.enabled ? Foreground : Muted);
    if(self.window.firstResponder==self) {
        NSBezierPath *focus=[NSBezierPath bezierPathWithRoundedRect:NSInsetRect(self.bounds,0.5,0.5) xRadius:4 yRadius:4];
        [Color(Accent) setStroke]; focus.lineWidth=1; [focus stroke];
    }
}
@end

NSButton *RemoteHostMakeButton(NSString *title, id target, SEL action, BOOL primary) {
    RemoteFlatButton *button = [[RemoteFlatButton alloc] initWithFrame:NSZeroRect];
    button.title = title; button.target = target; button.action = action;
    button.bordered = NO; button.primary = primary; button.font = [NSFont systemFontOfSize:13];
    button.focusRingType = NSFocusRingTypeNone; button.accessibilityLabel = title;
    return button;
}

@interface RemoteHostPanel () {
    RemoteHostPanelState *_state;
    NSScrollView *_addressScroll, *_tokenScroll;
    NSTextView *_addressText, *_tokenText;
    RemoteFlatButton *_copyAddress, *_copyToken, *_editToken, *_blacklist;
    RemoteFlatButton *_permissionButton, *_retryButton, *_logs;
    NSButton *_loginStartup, *_exchange;
    NSPopUpButton *_languagePicker;
    RemoteFlatButton *_files;
}
@end

@implementation RemoteHostPanel
- (void)openLogs:(id)sender {
    (void)sender;
    const auto directory = remote_log::Directory();
    std::error_code error;
    std::filesystem::create_directories(directory, error);
    NSString *path = [NSString stringWithUTF8String:directory.c_str()];
    if (path && !error) [[NSWorkspace sharedWorkspace] openURL:[NSURL fileURLWithPath:path isDirectory:YES]];
}
- (BOOL)isFlipped { return YES; }
- (BOOL)isOpaque { return YES; }
- (void)setFrameSize:(NSSize)size {
    [super setFrameSize:size]; self.needsLayout = YES; self.needsDisplay = YES;
}
- (RemoteFlatButton *)button:(NSString *)title target:(id)target action:(SEL)action primary:(BOOL)primary {
    RemoteFlatButton *button = (RemoteFlatButton *)RemoteHostMakeButton(title, target, action, primary);
    [self addSubview:button]; return button;
}
- (NSScrollView *)textField:(NSTextView * __strong *)output label:(NSString *)label {
    NSScrollView *scroll = [[NSScrollView alloc] initWithFrame:NSZeroRect];
    scroll.borderType = NSNoBorder; scroll.drawsBackground = NO;
    scroll.hasVerticalScroller = YES; scroll.hasHorizontalScroller = NO;
    scroll.autohidesScrollers = YES;
    NSTextView *text = [[NSTextView alloc] initWithFrame:NSMakeRect(0, 0, 450, 50)];
    text.editable = NO; text.selectable = YES; text.richText = NO;
    text.drawsBackground = NO; text.textColor = Color(Foreground);
    text.font = [NSFont monospacedSystemFontOfSize:12.5 weight:NSFontWeightRegular];
    text.textContainerInset = NSMakeSize(13, 9);
    text.minSize = NSMakeSize(0, 30); text.maxSize = NSMakeSize(CGFLOAT_MAX, CGFLOAT_MAX);
    text.verticallyResizable = YES; text.horizontallyResizable = NO;
    text.autoresizingMask = NSViewWidthSizable;
    text.textContainer.widthTracksTextView = YES;
    text.textContainer.heightTracksTextView = NO;
    text.textContainer.lineFragmentPadding = 0;
    text.accessibilityLabel = label;
    scroll.documentView = text; *output = text; [self addSubview:scroll]; return scroll;
}
- (instancetype)initWithTarget:(id)target permissions:(SEL)permissions retry:(SEL)retry
                     editToken:(SEL)editToken blacklist:(SEL)blacklist loginStartup:(SEL)loginStartup {
    if ((self = [super initWithFrame:NSMakeRect(0, 0, 840, 860)])) {
        self.appearance = [NSAppearance appearanceNamed:NSAppearanceNameDarkAqua];
        _state = [RemoteHostPanelState new];
        _addressScroll = [self textField:&_addressText label:RemoteHostText(@"连接地址，可选择并复制")];
        _tokenScroll = [self textField:&_tokenText label:RemoteHostText(@"完整配对令牌，可选择并复制")];
        _copyAddress = [self button:RemoteHostText(@"复制地址") target:self action:@selector(copyAddress:) primary:NO];
        _copyToken = [self button:RemoteHostText(@"复制令牌") target:self action:@selector(copyToken:) primary:NO];
        _editToken = [self button:RemoteHostText(@"修改令牌") target:target action:editToken primary:NO];
        _blacklist = [self button:RemoteHostText(@"黑名单管理") target:target action:blacklist primary:NO];
        _permissionButton = [self button:RemoteHostText(@"设置屏幕与输入权限") target:target action:permissions primary:YES];
        _retryButton = [self button:RemoteHostText(@"重试画面采集") target:target action:retry primary:NO];
        _logs = [self button:RemoteHostText(@"打开日志") target:self action:@selector(openLogs:) primary:NO];
        _logs.accessibilityLabel = RemoteHostText(@"打开 Remote Native 诊断日志目录");
        _loginStartup = [[RemoteFlatCheckbox alloc] initWithFrame:NSZeroRect];
        [_loginStartup setButtonType:NSButtonTypeSwitch];
        _loginStartup.title = RemoteHostText(@"登录后自动启动");
        _loginStartup.target = target; _loginStartup.action = loginStartup;
        _loginStartup.bordered = NO; _loginStartup.focusRingType = NSFocusRingTypeNone;
        _loginStartup.font = [NSFont systemFontOfSize:13];
    _loginStartup.contentTintColor = Color(Foreground);
    _loginStartup.attributedTitle = [[NSAttributedString alloc] initWithString:RemoteHostText(@"登录后自动启动")
        attributes:@{NSForegroundColorAttributeName:Color(Foreground), NSFontAttributeName:[NSFont systemFontOfSize:13]}];
    _loginStartup.attributedAlternateTitle = _loginStartup.attributedTitle;
    _loginStartup.appearance = [NSAppearance appearanceNamed:NSAppearanceNameDarkAqua];
        _loginStartup.accessibilityLabel = RemoteHostText(@"登录 macOS 后自动启动 Mac 端");
        _loginStartup.toolTip = RemoteHostText(@"进入 macOS 桌面后在菜单栏运行，不弹出窗口。");
        [self addSubview:_loginStartup];
        _exchange=[[RemoteFlatCheckbox alloc] initWithFrame:NSZeroRect];
        [_exchange setButtonType:NSButtonTypeSwitch]; _exchange.title=RemoteHostText(@"剪贴板与文件共享");
        _exchange.bordered=NO; _exchange.focusRingType=NSFocusRingTypeNone;
        _exchange.accessibilityLabel=RemoteHostText(@"允许已配对客户端同步剪贴板和传输文件");
        [self addSubview:_exchange];
        _files=[self button:RemoteHostText(@"文件传输…") target:nil action:nullptr primary:NO];
        _languagePicker = [[NSPopUpButton alloc] initWithFrame:NSZeroRect pullsDown:NO];
        _languagePicker.target = self; _languagePicker.action = @selector(changeLanguage:);
        _languagePicker.font = [NSFont systemFontOfSize:12];
        [self addSubview:_languagePicker];
        self.nextKeyView = _addressText; _addressText.nextKeyView = _copyAddress;
        _copyAddress.nextKeyView = _tokenText; _tokenText.nextKeyView = _copyToken;
        _copyToken.nextKeyView = _editToken; _editToken.nextKeyView = _blacklist;
        _blacklist.nextKeyView = _permissionButton; _permissionButton.nextKeyView = _retryButton;
        _retryButton.nextKeyView = _logs; _logs.nextKeyView = _loginStartup; _loginStartup.nextKeyView = _addressText;
        [self applyLocalization]; [self layout];
    }
    return self;
}
- (void)layout {
    [super layout];
    CGFloat width = self.bounds.size.width, height = self.bounds.size.height;
    CGFloat fieldWidth = width - 216, buttonX = width - 146;
    _languagePicker.frame = NSMakeRect(width - 364, 34, 172, 29);
    _addressScroll.frame = NSMakeRect(52, 246, fieldWidth, 38);
    _tokenScroll.frame = NSMakeRect(52, 319, fieldWidth, 52);
    _copyAddress.frame = NSMakeRect(buttonX, 247, 94, 36);
    _copyToken.frame = NSMakeRect(buttonX, 327, 94, 36);
    _editToken.frame = NSMakeRect(52, 391, 126, 36);
    _blacklist.frame = NSMakeRect(190, 391, 172, 36);
    _permissionButton.frame = NSMakeRect(32, height - 90, 190, 40);
    _retryButton.frame = NSMakeRect(234, height - 90, 144, 40);
    _logs.frame = NSMakeRect(390, height - 90, 112, 40);
    _loginStartup.frame = NSMakeRect(52, 628, 206, 25);
    _exchange.frame=NSMakeRect(52,665,244,25);
    _files.frame=NSMakeRect(310,660,118,32);
    for (NSScrollView *scroll in @[_addressScroll, _tokenScroll]) {
        NSTextView *text = (NSTextView *)scroll.documentView;
        NSSize size = scroll.contentSize;
        text.minSize = NSMakeSize(0, size.height);
        NSRect frame = text.frame; frame.size.width = size.width;
        frame.size.height = MAX(frame.size.height, size.height); text.frame = frame;
        text.textContainer.containerSize = NSMakeSize(MAX(1, size.width - 26), CGFLOAT_MAX);
    }
    self.needsDisplay = YES;
}
- (void)changeLanguage:(id)sender {
    RemoteHostSetLanguagePreference(static_cast<remote_i18n::Language>(_languagePicker.selectedItem.tag));
}
- (void)applyLocalization {
    _addressText.accessibilityLabel = RemoteHostText(@"连接地址，可选择并复制");
    _tokenText.accessibilityLabel = RemoteHostText(@"完整配对令牌，可选择并复制");
    _copyAddress.title = RemoteHostText(@"复制地址");
    _copyToken.title = RemoteHostText(@"复制令牌");
    _editToken.title = RemoteHostText(@"修改令牌");
    _permissionButton.title = RemoteHostText(@"设置屏幕与输入权限");
    _retryButton.title = RemoteHostText(@"重试画面采集");
    _logs.title = RemoteHostText(@"打开日志");
    _logs.accessibilityLabel = RemoteHostText(@"打开 Remote Native 诊断日志目录");
    _loginStartup.title = RemoteHostText(@"登录后自动启动");
    _loginStartup.accessibilityLabel = RemoteHostText(@"登录 macOS 后自动启动 Mac 端");
    _loginStartup.toolTip = RemoteHostText(@"进入 macOS 桌面后在菜单栏运行，不弹出窗口。");
    _exchange.title = RemoteHostText(@"剪贴板与文件共享");
    _exchange.accessibilityLabel = RemoteHostText(@"允许已配对客户端同步剪贴板和传输文件");
    _files.title = RemoteHostText(@"文件传输…");
    [_languagePicker removeAllItems];
    for (const auto language : {remote_i18n::Language::System, remote_i18n::Language::English, remote_i18n::Language::SimplifiedChinese}) {
        [_languagePicker addItemWithTitle:RemoteHostLanguageName(language)];
        _languagePicker.itemArray.lastObject.tag = static_cast<NSInteger>(language);
    }
    [_languagePicker selectItemWithTag:static_cast<NSInteger>(remote_i18n::GetLanguage())];
    _languagePicker.toolTip = RemoteHostText(@"语言");
    _languagePicker.accessibilityLabel = RemoteHostText(@"语言");
    [self updateState:_state];
    for (NSView *view in self.subviews) view.needsDisplay = YES;
}
- (void)updateState:(RemoteHostPanelState *)state {
    _state = state;
    // Do not replace text on every status tick: retain the user's active selection.
    if (![_addressText.string isEqualToString:state.address]) _addressText.string = state.address ?: @"";
    if (![_tokenText.string isEqualToString:state.token]) _tokenText.string = state.token ?: @"";
    _copyAddress.enabled = state.address.length > 0;
    _copyToken.enabled = state.token.length > 0;
    _editToken.enabled = state.token.length > 0;
    _blacklist.title = state.blockedCount ? [NSString stringWithFormat:RemoteHostText(@"黑名单管理 · %lu"), (unsigned long)state.blockedCount] : RemoteHostText(@"黑名单管理");
    _blacklist.needsDisplay = YES;
    _loginStartup.state = state.loginStartup ? NSControlStateValueOn : NSControlStateValueOff;
    _exchange.state = state.exchangeEnabled ? NSControlStateValueOn : NSControlStateValueOff;
    _exchange.needsDisplay=YES;
    self.needsDisplay = YES;
}
- (void)setExchangeTarget:(id)target toggle:(SEL)toggle files:(SEL)files {
    _exchange.target=target; _exchange.action=toggle; _files.target=target; _files.action=files;
}
- (void)copyValue:(NSString *)value button:(RemoteFlatButton *)button title:(NSString *)title {
    if (!value.length) return;
    NSPasteboard *pasteboard = NSPasteboard.generalPasteboard;
    [pasteboard clearContents];
    BOOL copied = [pasteboard setString:value forType:NSPasteboardTypeString];
    button.title = copied ? RemoteHostText(@"已复制") : RemoteHostText(@"复制失败"); button.needsDisplay = YES;
    __weak RemoteFlatButton *weakButton = button;
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 1400 * NSEC_PER_MSEC), dispatch_get_main_queue(), ^{
        RemoteFlatButton *live = weakButton;
        if (live) { live.title = RemoteHostText(title); live.needsDisplay = YES; }
    });
}
- (void)copyAddress:(id)sender { [self copyValue:_state.address button:_copyAddress title:RemoteHostText(@"复制地址")]; }
- (void)copyToken:(id)sender { [self copyValue:_state.token button:_copyToken title:RemoteHostText(@"复制令牌")]; }
- (void)drawRect:(NSRect)dirtyRect {
    [Color(Background) setFill]; NSRectFill(self.bounds);
    CGFloat width = self.bounds.size.width, height = self.bounds.size.height;
    static NSImage *brandIcon;
    static dispatch_once_t iconOnce;
    dispatch_once(&iconOnce, ^{
        brandIcon = [[NSImage alloc] initWithContentsOfFile:[NSBundle.mainBundle pathForResource:@"RemoteNative" ofType:@"icns"]];
    });
    [brandIcon drawInRect:NSMakeRect(29, 24, 52, 52) fromRect:NSZeroRect operation:NSCompositingOperationSourceOver fraction:1 respectFlipped:YES hints:nil];
    Text(RemoteHostBrand(), NSMakeRect(94, 24, 190, 34), 25, Foreground, NSFontWeightSemibold);
    Text(RemoteHostText(@"Mac 端 · 原生局域网远控"), NSMakeRect(95, 58, 250, 22), 12, Muted);
    Rounded(NSMakeRect(width - 180, 34, 148, 29), 14, _state.connected ? Soft : 0x192633);
    Dot(width - 164, 45, _state.connected ? Accent : Muted);
    Text(_state.connected ? RemoteHostText(@"Windows 已连接") : RemoteHostText(@"等待 Windows 连接"),
         NSMakeRect(width - 149, 40, 111, 19), 11, _state.connected ? Accent : Muted);
    Text(RemoteHostText(@"连接这台 Mac"), NSMakeRect(32, 105, width - 64, 34), 23, Foreground, NSFontWeightSemibold);
    Text(RemoteHostText(@"在 Windows 客户端填写地址与配对令牌，即可连接。"),
         NSMakeRect(32, 143, width - 64, 23), 13, Muted);

    Rounded(NSMakeRect(32, 178, width - 64, 314), 12, Panel, Line);
    Text(RemoteHostText(@"连接信息"), NSMakeRect(52, 195, 240, 26), 16, Foreground, NSFontWeightSemibold);
    Text(RemoteHostText(@"局域网 / H.264"), NSMakeRect(width - 197, 198, 145, 22), 11, Muted,
         NSFontWeightRegular, NSTextAlignmentRight);
    Text(RemoteHostText(@"连接地址"), NSMakeRect(52, 225, 300, 20), 12, Muted);
    Text(RemoteHostText(@"配对令牌"), NSMakeRect(52, 298, 300, 20), 12, Muted);
    Rounded(_addressScroll.frame, 7, Field, Line);
    Rounded(_tokenScroll.frame, 7, Field, Line);
    Text(RemoteHostText(@"令牌自动保存 · 可自定义 6–128 个字符"), NSMakeRect(52, 439, width - 104, 19), 11, Muted);
    Text(RemoteHostText(@"同一 IP 连续 10 次令牌验证失败将封禁，可在黑名单中解除。"),
         NSMakeRect(52, 465, width - 104, 19), 11, Muted);

    Rounded(NSMakeRect(32, 510, width - 64, 196), 12, Panel, Line);
    Text(RemoteHostText(@"权限与运行状态"), NSMakeRect(52, 526, width - 104, 26), 16, Foreground, NSFontWeightSemibold);
    CGFloat column = 52 + (width - 104) / 2;
    Text(RemoteHostText(@"屏幕录制"), NSMakeRect(52, 560, 126, 23), 13, Muted);
    Dot(186, 566, _state.screenGranted ? Accent : Warning);
    Text(_state.screenGranted ? RemoteHostText(@"已授权") : RemoteHostText(@"等待授权"), NSMakeRect(200, 560, 140, 23), 13,
         _state.screenGranted ? Accent : Warning);
    Text(RemoteHostText(@"辅助功能"), NSMakeRect(column, 560, 112, 23), 13, Muted);
    Dot(column + 120, 566, _state.inputGranted ? Accent : Warning);
    Text(_state.inputGranted ? RemoteHostText(@"已授权") : RemoteHostText(@"等待授权"), NSMakeRect(column + 134, 560, 140, 23), 13,
         _state.inputGranted ? Accent : Warning);
    NSString *capture = _state.capturing ? RemoteHostText(@"采集中") : (_state.starting ? RemoteHostText(@"正在启动采集") : RemoteHostText(@"采集未启动"));
    NSString *resolution = _state.width && _state.height ?
        [NSString stringWithFormat:RemoteHostText(@" · %u × %u · 目标 60 帧/秒"), _state.width, _state.height] : @"";
    Text([capture stringByAppendingString:resolution], NSMakeRect(52, 593, width - 104, 23), 12, Muted);
    Text(RemoteHostText(@"后台启动，不弹出连接窗口"), NSMakeRect(278, 633, width - 330, 22), 11, Muted);
    Text(RemoteHostText(_state.status) ?: RemoteHostText(@"正在启动…"), NSMakeRect(32, 718, width - 64, 25), 12, Muted);
    Text(RemoteHostText(_state.exchangeStatus) ?: RemoteHostText(@"复制后跨端粘贴 · 文件按需传输"), NSMakeRect(32,743,width-64,22),11,Muted);
    NSString *counter = [NSString stringWithFormat:RemoteHostText(@"已编码 %llu 帧 · 已替换 %llu 帧"),
        (unsigned long long)_state.encoded, (unsigned long long)_state.replaced];
    Text(counter, NSMakeRect(400, height - 78, width - 432, 24), 11, Muted,
         NSFontWeightRegular, NSTextAlignmentRight);
    Text(_state.preview ? RemoteHostText(@"界面预览 · 使用合成令牌与状态") :
         (_state.cursorBaked ? RemoteHostText(@"视频光标兼容模式 · 关闭窗口后仍在后台运行") :
                              RemoteHostText(@"Windows 本地光标 · 关闭窗口后仍在后台运行")),
         NSMakeRect(32, height - 27, width - 64, 21), 11, Muted);
}
@end

NSString *RemoteHostLocalizedStatus(NSString *message) {
    static NSDictionary<NSString *, NSString *> *translations;
    static dispatch_once_t once;
    dispatch_once(&once, ^{
        translations = @{
            @"Transport startup failed":@"连接服务启动失败，请查看诊断日志。",
            @"Connected":@"Windows 客户端已连接。",
            @"Pairing token updated":@"配对令牌已保存，请使用新令牌重新连接。",
            @"Listening; waiting for paired Windows client":@"连接服务已就绪，等待配对的 Windows 客户端。",
            @"Screen Recording permission required; click Request permissions":@"需要屏幕录制权限，请点击「设置屏幕与输入权限」。",
            @"Grant Screen Recording and Accessibility in System Settings; then retry capture":@"请在系统设置中允许屏幕录制和辅助功能，然后重试画面采集。",
            @"Capture already running":@"画面采集已在运行。",
            @"Streaming H.264 hardware":@"正在通过 H.264 硬件编码传送画面。",
            @"Waiting for screen permission/capture":@"客户端已连接，等待屏幕权限或画面采集。",
            @"Global cursor shape unavailable; using video cursor compatibility mode":@"暂时无法读取系统光标，已启用视频光标兼容模式。",
            @"Global cursor shape available; using local Windows cursor":@"已读取系统光标，使用 Windows 本地光标。",
            @"Transport unavailable; restart the host after fixing startup":@"连接服务不可用，修复启动问题后请重新启动 Mac 端。",
            @"Screen Recording permission missing":@"尚未获得屏幕录制权限。",
            @"Required hardware H.264 encoder could not start":@"无法启动所需的 H.264 硬件编码器。",
            @"Hardware H.264 encoder preparation failed":@"H.264 硬件编码器初始化失败。",
            @"ScreenCaptureKit NV12 hardware H.264 running":@"画面采集和 H.264 硬件编码已启动。"
        };
    });
    NSString *translated = translations[message]; if (translated) return RemoteHostText(translated);
    if ([message hasPrefix:@"Transport error:"]) return RemoteHostText(@"连接发生错误，请查看诊断日志。");
    if ([message hasPrefix:@"Screen source unavailable:"]) return RemoteHostText(@"暂时无法读取屏幕，请检查录屏权限或显示器状态。");
    if ([message hasPrefix:@"Capture attachment failed:"]) return RemoteHostText(@"无法连接屏幕采集输出，请查看诊断日志。");
    if ([message hasPrefix:@"Capture start error:"]) return RemoteHostText(@"画面采集启动失败，请查看诊断日志。");
    if ([message hasPrefix:@"Capture stopped:"]) return RemoteHostText(@"画面采集已停止，请检查权限后重试。");
    return RemoteHostText(@"运行状态已更新。");
}

NSString *RemoteHostConnectionAddress(NSString *bind) {
    // Display local addresses only; do not change the socket's actual binding.
    if (![bind hasPrefix:@"0.0.0.0:"] && ![bind hasPrefix:@"[::]:"]) return bind;
    NSString *port = [bind substringFromIndex:[bind rangeOfString:@":" options:NSBackwardsSearch].location + 1];
    struct ifaddrs *interfaces = nullptr;
    if (getifaddrs(&interfaces) != 0) return bind;
    NSString *selected = nil;
    for (struct ifaddrs *item = interfaces; item; item = item->ifa_next) {
        if (!item->ifa_addr || item->ifa_addr->sa_family != AF_INET ||
            !(item->ifa_flags & IFF_UP) || (item->ifa_flags & IFF_LOOPBACK)) continue;
        char address[INET_ADDRSTRLEN] = {};
        auto *ipv4 = reinterpret_cast<sockaddr_in *>(item->ifa_addr);
        if (!inet_ntop(AF_INET, &ipv4->sin_addr, address, sizeof(address))) continue;
        NSString *candidate = [NSString stringWithUTF8String:address];
        if ([candidate hasPrefix:@"169.254."]) continue;
        if (!selected) selected = candidate;
        if (strcmp(item->ifa_name, "en0") == 0) { selected = candidate; break; }
    }
    freeifaddrs(interfaces);
    return selected ? [NSString stringWithFormat:@"%@:%@", selected, port] : bind;
}

NSMenu *RemoteHostMakeLanguageMenu(id target, SEL action) {
    NSMenu *menu = [[NSMenu alloc] initWithTitle:RemoteHostText(@"语言")];
    for (const auto language : {remote_i18n::Language::System, remote_i18n::Language::English, remote_i18n::Language::SimplifiedChinese}) {
        NSMenuItem *item = [[NSMenuItem alloc] initWithTitle:RemoteHostLanguageName(language) action:action keyEquivalent:@""];
        item.target = target; item.tag = static_cast<NSInteger>(language);
        item.state = language == remote_i18n::GetLanguage() ? NSControlStateValueOn : NSControlStateValueOff;
        [menu addItem:item];
    }
    return menu;
}
void RemoteHostInstallApplicationMenu(void) {
    NSMenu *bar = [NSMenu new];
    NSMenuItem *application = [[NSMenuItem alloc] initWithTitle:RemoteHostBrand() action:nil keyEquivalent:@""];
    NSMenu *app = [[NSMenu alloc] initWithTitle:RemoteHostBrand()];
    [app addItemWithTitle:RemoteHostText(@"隐藏 Mac 端") action:@selector(hide:) keyEquivalent:@"h"];
    NSMenuItem *language = [[NSMenuItem alloc] initWithTitle:RemoteHostText(@"语言") action:nil keyEquivalent:@""];
    language.submenu = RemoteHostMakeLanguageMenu(NSApp.delegate, @selector(changeLanguage:));
    [app addItem:language];
    [app addItem:[NSMenuItem separatorItem]];
    [app addItemWithTitle:RemoteHostText(@"退出 Mac 端") action:@selector(terminate:) keyEquivalent:@"q"];
    application.submenu = app; [bar addItem:application];
    NSMenuItem *editItem = [[NSMenuItem alloc] initWithTitle:RemoteHostText(@"编辑") action:nil keyEquivalent:@""];
    NSMenu *edit = [[NSMenu alloc] initWithTitle:RemoteHostText(@"编辑")];
    [edit addItemWithTitle:RemoteHostText(@"复制") action:@selector(copy:) keyEquivalent:@"c"];
    [edit addItemWithTitle:RemoteHostText(@"全选") action:@selector(selectAll:) keyEquivalent:@"a"];
    editItem.submenu = edit; [bar addItem:editItem];
    NSMenuItem *windowItem = [[NSMenuItem alloc] initWithTitle:RemoteHostText(@"窗口") action:nil keyEquivalent:@""];
    NSMenu *window = [[NSMenu alloc] initWithTitle:RemoteHostText(@"窗口")];
    [window addItemWithTitle:RemoteHostText(@"最小化") action:@selector(performMiniaturize:) keyEquivalent:@"m"];
    [window addItemWithTitle:RemoteHostText(@"缩放") action:@selector(performZoom:) keyEquivalent:@""];
    [window addItemWithTitle:RemoteHostText(@"关闭窗口") action:@selector(performClose:) keyEquivalent:@"w"];
    windowItem.submenu = window; [bar addItem:windowItem];
    NSApp.mainMenu = bar;
}

BOOL RemoteHostRenderPreview(NSString *path) {
    // This path creates no host, token file, identity, endpoint, capture or permission request.
    [NSApplication sharedApplication];
    NSWindow *window = [[NSWindow alloc] initWithContentRect:NSMakeRect(0, 0, 840, 860)
        styleMask:NSWindowStyleMaskBorderless backing:NSBackingStoreBuffered defer:NO];
    window.appearance = [NSAppearance appearanceNamed:NSAppearanceNameDarkAqua];
    RemoteHostPanel *panel = [[RemoteHostPanel alloc] initWithTarget:nil permissions:nullptr retry:nullptr editToken:nullptr blacklist:nullptr loginStartup:nullptr];
    window.contentView = panel;
    RemoteHostPanelState *state = [RemoteHostPanelState new];
    state.address = @"192.0.2.10:47990";
    state.token = @"0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";
    state.fingerprint = @"abcdef0123456789abcdef0123456789abcdef0123456789abcdef0123456789";
    state.screenGranted = YES; state.inputGranted = YES; state.capturing = YES;
    state.connected = YES; state.preview = YES; state.width = 1920; state.height = 1080;
    state.status = RemoteHostText(@"画面采集和 H.264 硬件编码已启动。");
    state.encoded = 1280; state.replaced = 2;
    state.blockedCount = 1;
    state.loginStartup = YES;
    state.exchangeEnabled = YES;
    [panel updateState:state]; [panel layoutSubtreeIfNeeded];
    NSBitmapImageRep *bitmap = [panel bitmapImageRepForCachingDisplayInRect:panel.bounds];
    if (!bitmap) return NO;
    [panel cacheDisplayInRect:panel.bounds toBitmapImageRep:bitmap];
    NSData *png = [bitmap representationUsingType:NSBitmapImageFileTypePNG properties:@{}];
    return png && [png writeToFile:[path stringByExpandingTildeInPath] atomically:YES];
}
