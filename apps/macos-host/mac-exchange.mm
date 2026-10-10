#import "mac-exchange.h"
#import "host-ui.h"
#import "localization.h"
#import <ImageIO/ImageIO.h>
#import <ApplicationServices/ApplicationServices.h>
#include <algorithm>
#include <chrono>
#include <cstring>
#include <future>
#include <map>
#include "../../shared/diagnostic-log.h"

using namespace remote_exchange;
namespace {
NSString *const OriginType = @"dev.remote-native.clipboard-origin";
constexpr size_t ClipboardLimit = 32 * 1024 * 1024;
NSString *String(const std::string& s) { return [[NSString alloc] initWithBytes:s.data() length:s.size() encoding:NSUTF8StringEncoding] ?: @""; }
NSString *Path(const fs::path& p) { return [[NSString alloc] initWithUTF8String:p.string().c_str()] ?: @""; }
std::vector<uint8_t> Payload(NSData *data) {
    std::vector<uint8_t> bytes(data.length); if(data.length)memcpy(bytes.data(),data.bytes,data.length); return bytes;
}
uint64_t ClockMs() { return (uint64_t)std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count(); }
bool ValidPng(NSData *data) {
    if (!data.length || data.length > ClipboardLimit) return false;
    const uint8_t *p = (const uint8_t *)data.bytes;
    static const uint8_t signature[] = {137,80,78,71,13,10,26,10};
    if (data.length < 24 || memcmp(p, signature, 8)) return false;
    CGImageSourceRef source = CGImageSourceCreateWithData((__bridge CFDataRef)data, nullptr);
    if (!source) return false;
    NSDictionary *properties = CFBridgingRelease(CGImageSourceCopyPropertiesAtIndex(source,0,nullptr));
    CFRelease(source);
    uint64_t width=[properties[(__bridge NSString *)kCGImagePropertyPixelWidth] unsignedLongLongValue];
    uint64_t height=[properties[(__bridge NSString *)kCGImagePropertyPixelHeight] unsignedLongLongValue];
    return width && height && width <= 16384 && height <= 16384 && width*height <= 32*1024*1024;
}
std::future<std::vector<uint8_t>> ConvertTiff(NSData *tiff) {
    return std::async(std::launch::async,[tiff] {
        @autoreleasepool {
            std::vector<uint8_t> result;
            CGImageSourceRef source=CGImageSourceCreateWithData((__bridge CFDataRef)tiff,nullptr);
            if(!source)return result;
            NSDictionary *p=CFBridgingRelease(CGImageSourceCopyPropertiesAtIndex(source,0,nullptr));
            uint64_t w=[p[(__bridge NSString *)kCGImagePropertyPixelWidth] unsignedLongLongValue];
            uint64_t h=[p[(__bridge NSString *)kCGImagePropertyPixelHeight] unsignedLongLongValue];
            CGImageRef bitmap=w&&h&&w<=16384&&h<=16384&&w*h<=32*1024*1024 ? CGImageSourceCreateImageAtIndex(source,0,nullptr) : nullptr;
            CFRelease(source); if(!bitmap)return result;
            NSBitmapImageRep *rep=[[NSBitmapImageRep alloc] initWithCGImage:bitmap]; CFRelease(bitmap);
            NSData *png=[rep representationUsingType:NSBitmapImageFileTypePNG properties:@{}];
            if(ValidPng(png))result=Payload(png);
            return result;
        }
    });
}
NSPasteboardItem *Item(NSPasteboard *pb, NSString *origin) {
    [pb prepareForNewContentsWithOptions:NSPasteboardContentsCurrentHostOnly];
    NSPasteboardItem *item=[NSPasteboardItem new];
    [item setString:origin ?: @"remote" forType:OriginType];
    return item;
}
void Label(NSTextField *label, NSRect frame, NSString *text, CGFloat size=13) {
    label.frame=frame; label.stringValue=text ?: @""; label.font=[NSFont systemFontOfSize:size];
    label.textColor=NSColor.labelColor; label.lineBreakMode=NSLineBreakByTruncatingMiddle;
    label.maximumNumberOfLines=1; label.cell.usesSingleLineMode=YES;
}
NSString *BytesText(uint64_t bytes) {
    NSArray<NSString *> *units=@[@"B",@"KB",@"MB",@"GB",@"TB"];
    double value=(double)bytes; NSUInteger unit=0;
    while(value>=1024 && unit+1<units.count) { value/=1024; ++unit; }
    return unit ? [NSString stringWithFormat:@"%.1f %@",value,units[unit]] : [NSString stringWithFormat:@"%llu B",(unsigned long long)bytes];
}
}

bool RemoteMacInstallClipboard(NSPasteboard *pb, const ClipboardPacket& packet, NSString *origin) {
    if (!pb || packet.data.size()>ClipboardLimit) return false;
    if (packet.kind == ClipboardKind::Text) {
        NSString *text=[[NSString alloc] initWithBytes:packet.data.data() length:packet.data.size() encoding:NSUTF8StringEncoding];
        if (!text) return false;
        NSPasteboardItem *item=Item(pb,origin); [item setString:text forType:NSPasteboardTypeString];
        return [pb writeObjects:@[item]];
    }
    if (packet.kind == ClipboardKind::Png) {
        NSData *data=[NSData dataWithBytes:packet.data.data() length:packet.data.size()];
        if (!ValidPng(data)) return false;
        NSPasteboardItem *item=Item(pb,origin); [item setData:data forType:NSPasteboardTypePNG];
        return [pb writeObjects:@[item]];
    }
    return false;
}
bool RemoteMacInstallFiles(NSPasteboard *pb, const std::vector<fs::path>& files, NSString *origin) {
    if (!pb || files.empty() || files.size()>8192) return false;
    NSMutableArray<NSURL *> *urls=[NSMutableArray new];
    for (const auto& file:files) {
        NSString *path=Path(file);
        if (!path.length || !file.is_absolute()) return false;
        // Session already validated the completed paths. Do not stat thousands
        // of transferred items on the input/UI thread.
        [urls addObject:[NSURL fileURLWithPath:path isDirectory:NO]];
    }
    [pb prepareForNewContentsWithOptions:NSPasteboardContentsCurrentHostOnly];
    BOOL okay=[pb writeObjects:urls];
    // An additional private item tags the operation without replacing URL objects.
    if (okay) [pb setString:origin ?: @"remote" forType:OriginType];
    return okay;
}
std::vector<fs::path> RemoteMacClipboardFiles(NSPasteboard *pb) {
    NSArray<NSURL *> *urls=[pb readObjectsForClasses:@[NSURL.class] options:@{NSPasteboardURLReadingFileURLsOnlyKey:@YES}];
    std::vector<fs::path> paths;
    for (NSURL *url in urls) {
        if (paths.size()>=8192) break;
        if (url.isFileURL && url.fileSystemRepresentation) paths.emplace_back(url.fileSystemRepresentation);
    }
    return paths;
}

@interface RemoteMacDropView : NSView <NSDraggingDestination>
@property(copy) void (^send)(NSArray<NSURL *> *);
@property BOOL canSend;
@end
@implementation RemoteMacDropView
- (instancetype)initWithFrame:(NSRect)frame {
    if((self=[super initWithFrame:frame])) [self registerForDraggedTypes:@[NSPasteboardTypeFileURL]];
    return self;
}
- (void)drawRect:(NSRect)rect {
    [[NSColor colorWithSRGBRed:.07 green:.13 blue:.17 alpha:1] setFill];
    [[NSBezierPath bezierPathWithRoundedRect:NSInsetRect(self.bounds,.5,.5) xRadius:9 yRadius:9] fill];
    [RemoteHostText(@"拖入 Mac 文件，发送到 Windows 接收目录") drawInRect:NSInsetRect(self.bounds,16,13)
        withAttributes:@{NSFontAttributeName:[NSFont systemFontOfSize:13],NSForegroundColorAttributeName:NSColor.secondaryLabelColor}];
}
- (NSDragOperation)draggingEntered:(id<NSDraggingInfo>)sender { return self.canSend ? NSDragOperationCopy : NSDragOperationNone; }
- (BOOL)performDragOperation:(id<NSDraggingInfo>)sender {
    if (!self.canSend || !self.send) return NO;
    NSArray *urls=[sender.draggingPasteboard readObjectsForClasses:@[NSURL.class] options:@{NSPasteboardURLReadingFileURLsOnlyKey:@YES}];
    if (!urls.count) return NO; self.send(urls); return YES;
}
@end

@interface RemoteMacTransferBackground : NSView
@end
@implementation RemoteMacTransferBackground
- (BOOL)isOpaque { return YES; }
- (void)drawRect:(NSRect)rect {
    [[NSColor colorWithSRGBRed:.04 green:.07 blue:.10 alpha:1] setFill]; NSRectFill(self.bounds);
}
@end

@interface RemoteMacTransferController : NSObject <NSTableViewDataSource,NSTableViewDelegate,NSWindowDelegate> {
@public
    Session *_session;
    NSWindow *_window;
    NSTableView *_table;
    NSTextField *_directory, *_summary, *_heading;
    NSButton *_send, *_pause, *_resume, *_cancel, *_choose, *_open;
    RemoteMacDropView *_drop;
    std::vector<Transfer> _rows;
}
- (instancetype)initWithSession:(Session *)session;
- (void)refresh;
- (void)show;
@end
@implementation RemoteMacTransferController
- (instancetype)initWithSession:(Session *)session {
    if(!(self=[super init])) return nil;
    _session=session;
    _window=[[NSWindow alloc] initWithContentRect:NSMakeRect(0,0,800,520)
        styleMask:NSWindowStyleMaskTitled|NSWindowStyleMaskClosable|NSWindowStyleMaskMiniaturizable|NSWindowStyleMaskResizable
        backing:NSBackingStoreBuffered defer:NO];
    _window.title=RemoteHostText(@"光梭 · 文件传输"); _window.releasedWhenClosed=NO; _window.delegate=self;
    _window.contentMinSize=NSMakeSize(720,480); _window.appearance=[NSAppearance appearanceNamed:NSAppearanceNameDarkAqua];
    _window.backgroundColor=[NSColor colorWithSRGBRed:.04 green:.07 blue:.10 alpha:1];
    NSView *view=[[RemoteMacTransferBackground alloc] initWithFrame:NSMakeRect(0,0,800,520)];
    _window.contentView=view;
    _heading=[NSTextField labelWithString:@""]; Label(_heading,NSMakeRect(24,464,752,28),RemoteHostText(@"双向文件传输"),22); _heading.autoresizingMask=NSViewWidthSizable|NSViewMinYMargin; [view addSubview:_heading];
    _directory=[NSTextField labelWithString:@""]; Label(_directory,NSMakeRect(24,429,516,22),@""); _directory.autoresizingMask=NSViewWidthSizable|NSViewMinYMargin; [view addSubview:_directory];
    _choose=RemoteHostMakeButton(RemoteHostText(@"接收目录…"),self,@selector(chooseDirectory:),NO); _choose.frame=NSMakeRect(552,424,132,32); _choose.autoresizingMask=NSViewMinXMargin|NSViewMinYMargin; [view addSubview:_choose];
    _open=RemoteHostMakeButton(RemoteHostText(@"打开"),self,@selector(openDirectory:),NO); _open.frame=NSMakeRect(694,424,82,32); _open.autoresizingMask=NSViewMinXMargin|NSViewMinYMargin; [view addSubview:_open];
    _send=RemoteHostMakeButton(RemoteHostText(@"选择文件发送…"),self,@selector(sendFiles:),YES); _send.frame=NSMakeRect(24,373,146,36); _send.autoresizingMask=NSViewMinYMargin; [view addSubview:_send];
    _pause=RemoteHostMakeButton(RemoteHostText(@"暂停"),self,@selector(pause:),NO); _pause.frame=NSMakeRect(194,373,84,36); _pause.autoresizingMask=NSViewMinYMargin; [view addSubview:_pause];
    _resume=RemoteHostMakeButton(RemoteHostText(@"继续"),self,@selector(resume:),NO); _resume.frame=NSMakeRect(290,373,84,36); _resume.autoresizingMask=NSViewMinYMargin; [view addSubview:_resume];
    _cancel=RemoteHostMakeButton(RemoteHostText(@"取消"),self,@selector(cancel:),NO); _cancel.frame=NSMakeRect(386,373,84,36); _cancel.autoresizingMask=NSViewMinYMargin; [view addSubview:_cancel];
    _drop=[[RemoteMacDropView alloc] initWithFrame:NSMakeRect(24,311,752,48)]; _drop.autoresizingMask=NSViewWidthSizable|NSViewMinYMargin; [view addSubview:_drop];
    __weak RemoteMacTransferController *weakSelf=self;
    _drop.send=^(NSArray<NSURL *> *urls) { [weakSelf sendUrls:urls]; };
    NSScrollView *scroll=[[NSScrollView alloc] initWithFrame:NSMakeRect(24,58,752,238)]; scroll.autoresizingMask=NSViewWidthSizable|NSViewHeightSizable; scroll.hasVerticalScroller=YES; scroll.autohidesScrollers=YES;
    _table=[[NSTableView alloc] initWithFrame:scroll.bounds]; _table.delegate=self; _table.dataSource=self; _table.rowHeight=32; _table.allowsMultipleSelection=NO;
    _table.usesAlternatingRowBackgroundColors=YES; _table.style=NSTableViewStyleFullWidth;
    for(NSArray *spec in @[@[@"name",RemoteHostText(@"文件"),@270],@[@"direction",RemoteHostText(@"方向"),@82],@[@"progress",RemoteHostText(@"进度"),@135],@[@"speed",RemoteHostText(@"速度"),@100],@[@"state",RemoteHostText(@"状态"),@130]]) {
        NSTableColumn *column=[[NSTableColumn alloc] initWithIdentifier:spec[0]]; column.title=spec[1]; column.width=[spec[2] doubleValue]; [_table addTableColumn:column];
    }
    scroll.documentView=_table; [view addSubview:scroll];
    _summary=[NSTextField labelWithString:@""]; Label(_summary,NSMakeRect(24,20,752,24),RemoteHostText(@"配对连接后可发送文件或文件夹。"),12); _summary.autoresizingMask=NSViewWidthSizable; [view addSubview:_summary];
    [NSNotificationCenter.defaultCenter addObserver:self selector:@selector(languageChanged:) name:RemoteHostLanguageChangedNotification object:nil];
    [NSNotificationCenter.defaultCenter addObserver:self selector:@selector(languageChanged:) name:NSCurrentLocaleDidChangeNotification object:nil];
    [self languageChanged:nil]; [_window center]; return self;
}
- (void)dealloc { [NSNotificationCenter.defaultCenter removeObserver:self]; }
- (void)languageChanged:(NSNotification *)note {
    (void)note;
    _window.title = RemoteHostText(@"光梭 · 文件传输");
    _heading.stringValue = RemoteHostText(@"双向文件传输");
    _choose.title = RemoteHostText(@"接收目录…"); _open.title = RemoteHostText(@"打开");
    _send.title = RemoteHostText(@"选择文件发送…"); _pause.title = RemoteHostText(@"暂停");
    _resume.title = RemoteHostText(@"继续"); _cancel.title = RemoteHostText(@"取消");
    NSDictionary *titles = @{@"name":@"文件",@"direction":@"方向",@"progress":@"进度",@"speed":@"速度",@"state":@"状态"};
    for (NSTableColumn *column in _table.tableColumns) column.title = RemoteHostText(titles[column.identifier]);
    _drop.needsDisplay = YES;
    for (NSView *view in _window.contentView.subviews) view.needsDisplay = YES;
    [self refresh];
}
- (void)show { [self refresh]; [NSApp activateIgnoringOtherApps:YES]; [_window makeKeyAndOrderFront:nil]; }
- (BOOL)windowShouldClose:(NSWindow *)sender { [sender orderOut:nil]; return NO; }
- (void)chooseDirectory:(id)sender {
    NSOpenPanel *panel=[NSOpenPanel openPanel]; panel.canChooseFiles=NO; panel.canChooseDirectories=YES; panel.canCreateDirectories=YES; panel.prompt=RemoteHostText(@"选择接收目录");
    [panel beginSheetModalForWindow:_window completionHandler:^(NSModalResponse result) {
        if(result!=NSModalResponseOK || !panel.URL.fileSystemRepresentation) return;
        self->_session->SetReceiveDirectory(fs::path(panel.URL.fileSystemRepresentation));
        [NSUserDefaults.standardUserDefaults setObject:panel.URL.path forKey:@"RemoteReceiveDirectory"];
        [self refresh];
    }];
}
- (void)openDirectory:(id)sender {
    fs::path directory=_session->ReceiveDirectory();
    dispatch_async(dispatch_get_global_queue(QOS_CLASS_UTILITY,0), ^{
        std::error_code error; fs::create_directories(directory,error);
        NSString *path=Path(directory);
        dispatch_async(dispatch_get_main_queue(), ^{
            if(error)self->_summary.stringValue=[RemoteHostText(@"接收目录不可用：") stringByAppendingString:String(error.message())];
            else [NSWorkspace.sharedWorkspace openURL:[NSURL fileURLWithPath:path isDirectory:YES]];
        });
    });
}
- (void)sendFiles:(id)sender {
    NSOpenPanel *panel=[NSOpenPanel openPanel]; panel.canChooseFiles=YES; panel.canChooseDirectories=YES; panel.allowsMultipleSelection=YES; panel.prompt=RemoteHostText(@"发送到 Windows");
    [panel beginSheetModalForWindow:_window completionHandler:^(NSModalResponse result) { if(result==NSModalResponseOK) [self sendUrls:panel.URLs]; }];
}
- (void)sendUrls:(NSArray<NSURL *> *)urls {
    std::vector<fs::path> files; for(NSURL *url in urls) if(url.isFileURL && url.fileSystemRepresentation) files.emplace_back(url.fileSystemRepresentation);
    if(!files.empty()) _session->SendFiles(std::move(files)); [self refresh];
}
- (uint64_t)selected { NSInteger index=_table.selectedRow; return index>=0 && (size_t)index<_rows.size() ? _rows[index].id : 0; }
- (void)pause:(id)sender { uint64_t id=[self selected]; if(id) _session->Pause(id); [self refresh]; }
- (void)resume:(id)sender { uint64_t id=[self selected]; if(id) _session->Resume(id); [self refresh]; }
- (void)cancel:(id)sender { uint64_t id=[self selected]; if(id) _session->Cancel(id); [self refresh]; }
- (NSInteger)numberOfRowsInTableView:(NSTableView *)tableView { return (NSInteger)_rows.size(); }
- (NSView *)tableView:(NSTableView *)tableView viewForTableColumn:(NSTableColumn *)column row:(NSInteger)row {
    if(row<0 || (size_t)row>=_rows.size()) return nil;
    const Transfer& t=_rows[row]; NSString *value;
    if([column.identifier isEqualToString:@"name"]) value=String(t.name);
    else if([column.identifier isEqualToString:@"direction"]) value=t.outgoing ? @"→ Windows" : @"→ Mac";
    else if([column.identifier isEqualToString:@"progress"]) value=[NSString stringWithFormat:@"%@ / %@",BytesText(t.bytes),BytesText(t.total)];
    else if([column.identifier isEqualToString:@"speed"]) value=t.state==TransferState::Running ? [BytesText((uint64_t)std::max(0.0,t.bytesPerSecond)) stringByAppendingString:@"/s"] : @"—";
    else value=RemoteHostText(t.error.empty() ? String(StateLabel(t.state)) : String(t.error));
    NSTextField *label=[NSTextField labelWithString:value ?: @""]; label.font=[NSFont systemFontOfSize:12]; label.lineBreakMode=NSLineBreakByTruncatingMiddle; label.toolTip=value; return label;
}
- (void)tableViewSelectionDidChange:(NSNotification *)note { [self refreshButtons]; }
- (void)refreshButtons {
    NSInteger i=_table.selectedRow; bool valid=i>=0&&(size_t)i<_rows.size();
    TransferState state=valid ? _rows[i].state : TransferState::Complete;
    _pause.enabled=valid && (state==TransferState::Running || state==TransferState::Queued);
    _resume.enabled=valid && state==TransferState::Paused;
    _cancel.enabled=valid && (state==TransferState::Running || state==TransferState::Queued || state==TransferState::Paused);
}
- (void)refresh {
    Snapshot snapshot=_session->GetSnapshot(); _rows=std::move(snapshot.transfers);
    _directory.stringValue=[RemoteHostText(@"Mac 接收目录：") stringByAppendingString:Path(_session->ReceiveDirectory())];
    _directory.toolTip=_directory.stringValue;
    _send.enabled=snapshot.connected&&snapshot.enabled; _drop.canSend=_send.enabled;
    _summary.stringValue=snapshot.message.empty() ? (snapshot.connected ? RemoteHostText(@"文件复制后粘贴才开始传输 · 源文件保持不变") : RemoteHostText(@"等待已配对的 Windows 连接")) : RemoteHostText(String(snapshot.message));
    NSInteger row=_table.selectedRow; [_table reloadData];
    if(row>=0 && (size_t)row<_rows.size()) [_table selectRowIndexes:[NSIndexSet indexSetWithIndex:row] byExtendingSelection:NO];
    [self refreshButtons];
}
@end

bool RemoteMacRenderTransfersPreview(Session& session,NSString *path) {
    [NSApplication sharedApplication];
    RemoteMacTransferController *controller=[[RemoteMacTransferController alloc] initWithSession:&session];
    NSView *view=controller->_window.contentView;
    [view layoutSubtreeIfNeeded];
    NSBitmapImageRep *bitmap=[view bitmapImageRepForCachingDisplayInRect:view.bounds];
    if(!bitmap)return false;
    [view cacheDisplayInRect:view.bounds toBitmapImageRep:bitmap];
    NSData *png=[bitmap representationUsingType:NSBitmapImageFileTypePNG properties:@{}];
    return png && [png writeToFile:path atomically:YES];
}

struct RemoteMacExchange::Impl {
    Session& session;
    Paste paste;
    __strong NSPasteboard *pb;
    __strong NSString *origin, *cache, *message;
    __strong RemoteMacTransferController *panel=nil;
    bool connected=false, enabled=true;
    NSInteger revision=0, installedRevision=0;
    uint64_t installedId=0, pollAt=0, panelAt=0;
    uint64_t epoch=0, imageEpoch=0;
    NSInteger imageRevision=0;
    std::future<std::vector<uint8_t>> image;
    __strong NSData *nextTiff=nil;
    NSInteger nextImageRevision=0;
    uint64_t nextImageEpoch=0;
    struct Pending {
        uint64_t id=0, transfer=0, started=0, focusChecked=0, transferred=0;
        uint32_t flags=0;
        NSInteger revision=0;
        pid_t pid=0;
        AXUIElementRef window=nullptr;
    } pending;
    Impl(Session& value,Paste callback,NSPasteboard *board,NSString *directory)
        :session(value),paste(std::move(callback)),pb(board ?: NSPasteboard.generalPasteboard),
         origin(NSUUID.UUID.UUIDString),cache(directory ?: [NSHomeDirectory() stringByAppendingPathComponent:@"Library/Caches/RemoteNative/Clipboard"]),message(@"") {
        revision=pb.changeCount;
    }
    ~Impl() { if(pending.window) CFRelease(pending.window); }
    AXUIElementRef FocusWindow(pid_t pid) {
        if(pid<=0) return nullptr;
        AXUIElementRef app=AXUIElementCreateApplication(pid); CFTypeRef value=nullptr;
        AXUIElementSetMessagingTimeout(app,.05f);
        AXUIElementCopyAttributeValue(app,kAXFocusedWindowAttribute,&value); CFRelease(app);
        return value && CFGetTypeID(value)==AXUIElementGetTypeID() ? (AXUIElementRef)value : (value ? (CFRelease(value),nullptr) : nullptr);
    }
    bool FocusValid() {
        if(NSWorkspace.sharedWorkspace.frontmostApplication.processIdentifier!=pending.pid) return false;
        if(!pending.window) return true;
        AXUIElementRef window=FocusWindow(pending.pid); bool same=window && CFEqual(window,pending.window);
        if(window)CFRelease(window); return same;
    }
    void Clear() { if(pending.window) CFRelease(pending.window); pending=Pending{}; }
    void Cancel(NSString *reason) {
        bool active=pending.id!=0;
        if(active)remote_log::Write(remote_log::Level::Info,"mac-clipboard","paste cancelled id="+std::to_string(pending.id)+" transfer="+std::to_string(pending.transfer)+" error_class="+remote_log::ErrorClass(reason.UTF8String ? reason.UTF8String : ""));
        if(pending.id) { session.ClipboardInstalled(pending.id,false); if(pending.transfer)session.Cancel(pending.transfer); }
        Clear(); if(active && reason.length)message=reason;
    }
    void Finish() {
        if(!pending.id || !FocusValid() || pb.changeCount!=installedRevision) { Cancel(RemoteHostText(@"粘贴已取消：目标窗口或剪贴板已变化")); return; }
        uint32_t flags=pending.flags; Clear(); message=@""; if(paste)paste(flags);
    }
};

RemoteMacExchange::RemoteMacExchange(Session& session,Paste paste,NSPasteboard *pb,NSString *cache)
    :impl_(std::make_unique<Impl>(session,std::move(paste),pb,cache)) {}
RemoteMacExchange::~RemoteMacExchange() = default;
bool RemoteMacExchange::Enabled() const { return impl_->enabled; }
NSString *RemoteMacExchange::Status() const { return RemoteHostText(impl_->message); }
void RemoteMacExchange::SetConnected(bool connected) {
    auto& i=*impl_; if(i.connected==connected)return; i.connected=connected;
    remote_log::Write(remote_log::Level::Info,"mac-clipboard",connected?"sharing channel connected":"sharing channel disconnected");
    ++i.epoch;
    // No session-start upload of the user's old clipboard, and no background reads.
    i.revision=i.pb.changeCount; i.installedId=0;
    if(!connected) { i.nextTiff=nil; i.Cancel(nil); }
}
void RemoteMacExchange::SetEnabled(bool enabled) {
    auto& i=*impl_; i.enabled=enabled; i.session.SetEnabled(enabled);
    remote_log::Write(remote_log::Level::Info,"mac-clipboard",enabled?"sharing enabled":"sharing disabled");
    ++i.epoch;
    i.revision=i.pb.changeCount; i.installedId=0; if(!enabled) { i.nextTiff=nil; i.Cancel(nil); }
    [NSUserDefaults.standardUserDefaults setBool:!enabled forKey:@"RemoteExchangeDisabled"];
}
void RemoteMacExchange::RequestPaste(uint64_t id,uint32_t flags) {
    auto& i=*impl_; i.Cancel(nil);
    if(!id || !i.connected || !i.enabled) { remote_log::Write(remote_log::Level::Warning,"mac-clipboard","paste rejected; sharing unavailable"); i.session.ClipboardInstalled(id,false); return; }
    remote_log::Write(remote_log::Level::Info,"mac-clipboard","paste requested id="+std::to_string(id));
    i.pending.id=id; i.pending.flags=flags; i.pending.started=ClockMs(); i.pending.revision=i.pb.changeCount;
    i.pending.pid=NSWorkspace.sharedWorkspace.frontmostApplication.processIdentifier;
    i.pending.window=i.FocusWindow(i.pending.pid);
    if(i.installedId==id && i.installedRevision==i.pb.changeCount) i.Finish();
    else i.message=@"正在准备粘贴内容…";
}
void RemoteMacExchange::CancelPaste(NSString *reason) { impl_->Cancel(reason); }
void RemoteMacExchange::ShowTransfers() {
    auto& i=*impl_; if(!i.panel)i.panel=[[RemoteMacTransferController alloc] initWithSession:&i.session]; [i.panel show];
}
void RemoteMacExchange::Tick(bool transportReady) {
    auto& i=*impl_; uint64_t now=ClockMs();
    Snapshot snapshot=i.session.GetSnapshot(); SetConnected(transportReady && snapshot.connected);
    if(i.panel && now-i.panelAt>=500) { i.panelAt=now; [i.panel refresh]; }
    if(!i.connected || !i.enabled)return;
    if(i.image.valid() && i.image.wait_for(std::chrono::seconds(0))==std::future_status::ready) {
        auto png=i.image.get();
        if(!png.empty() && i.imageEpoch==i.epoch && i.imageRevision==i.pb.changeCount) {
            i.installedId=i.session.PublishClipboard(ClipboardKind::Png,std::move(png)); i.installedRevision=i.pb.changeCount;
        }
    }
    if(!i.image.valid() && i.nextTiff) {
        if(i.nextImageRevision==i.pb.changeCount && i.nextImageEpoch==i.epoch) {
            i.imageRevision=i.nextImageRevision; i.imageEpoch=i.nextImageEpoch; i.image=ConvertTiff(i.nextTiff);
        }
        i.nextTiff=nil;
    }
    if(i.pending.id && now-i.pending.focusChecked>=100) {
        i.pending.focusChecked=now;
        bool alive=false, paused=false;
        if(i.pending.transfer) for(const auto& transfer:snapshot.transfers) if(transfer.id==i.pending.transfer) {
            paused=transfer.state==TransferState::Paused;
            if(transfer.bytes!=i.pending.transferred) { i.pending.transferred=transfer.bytes; i.pending.started=now; }
            alive=transfer.state==TransferState::Complete;
            break;
        }
        // Large transfers can run for hours; only inactivity times out. A user's
        // explicit pause does not expire the pending paste.
        if(!i.FocusValid() || (!paused && !alive && now-i.pending.started>60000))i.Cancel(RemoteHostText(@"粘贴已取消：目标窗口变化或传输等待超时"));
    }
    bool localChanged=false;
    if(now-i.pollAt>=33) {
        i.pollAt=now; NSInteger count=i.pb.changeCount;
        if(count!=i.revision) {
            i.revision=count; localChanged=true; i.installedId=0;
            if(i.pending.id)i.Cancel(RemoteHostText(@"粘贴已取消：Mac 剪贴板已有新内容"));
            auto files=RemoteMacClipboardFiles(i.pb);
            if(!files.empty()) { i.installedId=i.session.OfferFiles(std::move(files)); i.installedRevision=count; }
            else {
                NSData *png=[i.pb dataForType:NSPasteboardTypePNG];
                if(ValidPng(png)) {
                    i.installedId=i.session.PublishClipboard(ClipboardKind::Png,Payload(png)); i.installedRevision=count;
                } else if([i.pb.types containsObject:NSPasteboardTypeTIFF]) {
                    NSData *tiff=[i.pb dataForType:NSPasteboardTypeTIFF];
                    if(tiff.length && tiff.length<=ClipboardLimit) {
                        if(i.image.valid()) { i.nextTiff=tiff; i.nextImageRevision=count; i.nextImageEpoch=i.epoch; }
                        else { i.imageRevision=count; i.imageEpoch=i.epoch; i.image=ConvertTiff(tiff); }
                    }
                } else {
                    NSString *text=[i.pb stringForType:NSPasteboardTypeString];
                    NSData *data=[text dataUsingEncoding:NSUTF8StringEncoding];
                    if(data && data.length<=ClipboardLimit) {
                        i.installedId=i.session.PublishClipboard(ClipboardKind::Text,Payload(data)); i.installedRevision=count;
                    }
                }
            }
        }
    }
    ClipboardPacket packet;
    for(int n=0;n<8 && i.session.TakeClipboard(packet);++n) {
        bool okay=!localChanged && i.pb.changeCount==i.revision && RemoteMacInstallClipboard(i.pb,packet,i.origin);
        i.session.ClipboardInstalled(packet.id,okay);
        if(okay) { i.revision=i.pb.changeCount; i.installedRevision=i.revision; i.installedId=packet.id; }
        if(okay && i.pending.id && i.pending.id!=packet.id)i.Cancel(RemoteHostText(@"粘贴已取消：收到新的复制内容"));
        if(i.pending.id==packet.id) { if(okay)i.Finish(); else i.Cancel(RemoteHostText(@"粘贴内容安装失败或已被新复制取代")); }
    }
    CompletedFiles completed;
    while(i.session.TakeCompletedFiles(completed)) {
        if(!i.pending.id || completed.transferId!=i.pending.transfer)continue;
        if(i.pb.changeCount!=i.pending.revision || !i.FocusValid()) { i.Cancel(RemoteHostText(@"文件粘贴已取消：目标窗口或剪贴板变化")); continue; }
        bool okay=RemoteMacInstallFiles(i.pb,completed.files,i.origin);
        i.session.ClipboardInstalled(i.pending.id,okay);
        if(okay) { i.revision=i.pb.changeCount; i.installedRevision=i.revision; i.installedId=i.pending.id; i.Finish(); }
        else i.Cancel(RemoteHostText(@"文件粘贴准备失败"));
    }
    if(i.pending.id && !i.pending.transfer && snapshot.remoteOffer.id==i.pending.id) {
        NSString *destination=[i.cache stringByAppendingPathComponent:[NSString stringWithFormat:@"%llu-%@",(unsigned long long)i.pending.id,NSUUID.UUID.UUIDString]];
        i.pending.transfer=i.session.RequestFiles(i.pending.id,fs::path(destination.fileSystemRepresentation));
        if(!i.pending.transfer)i.Cancel(RemoteHostText(@"无法开始文件粘贴传输"));
        else i.message=@"正在传输文件，完成后粘贴到当前窗口…";
    }
    if(i.pending.transfer) for(const auto& transfer:snapshot.transfers) if(transfer.id==i.pending.transfer && (transfer.state==TransferState::Error || transfer.state==TransferState::Cancelled)) {
        i.Cancel(transfer.error.empty() ? RemoteHostText(@"文件粘贴已取消") : String(transfer.error)); break;
    }
}
