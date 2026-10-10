#import "../mac-exchange.h"
#import "../host-ui.h"
#include <chrono>
#include <fstream>
#include <iostream>
#include <thread>

using namespace remote_exchange;
static int passed=0,failed=0;
static void Check(bool result,const char *name) {
    std::cout<<(result?"PASS ":"FAIL ")<<name<<"\n"; result ? ++passed : ++failed;
}
template<class Predicate> static bool Wait(Predicate predicate,RemoteMacExchange *native=nullptr,int milliseconds=3000) {
    auto end=std::chrono::steady_clock::now()+std::chrono::milliseconds(milliseconds);
    do { if(native)native->Tick(); if(predicate())return true; std::this_thread::sleep_for(std::chrono::milliseconds(5)); }
    while(std::chrono::steady_clock::now()<end); return false;
}
static std::vector<uint8_t> Bytes(NSString *text) {
    NSData *data=[text dataUsingEncoding:NSUTF8StringEncoding]; const uint8_t *p=(const uint8_t *)data.bytes;
    return std::vector<uint8_t>(p,p+data.length);
}
int main() { @autoreleasepool {
    // Never read or mutate NSPasteboard.generalPasteboard; all fixtures are ours.
    NSPasteboard *pb=[NSPasteboard pasteboardWithUniqueName];
    NSPasteboard *unrelated=[NSPasteboard pasteboardWithUniqueName];
    [unrelated setString:@"sentinel" forType:NSPasteboardTypeString];
    NSString *sample=@"中文代码 👨‍💻\nline 2\r\n\t 终点";
    ClipboardPacket packet{42,ClipboardKind::Text,Bytes(sample)};
    NSInteger before=pb.changeCount;
    Check(RemoteMacInstallClipboard(pb,packet,@"probe-origin"),"install UTF-8 text");
    Check([[pb stringForType:NSPasteboardTypeString] isEqualToString:sample],"Unicode and newline exact round trip");
    Check(pb.changeCount>before,"native clipboard revision advanced");
    Check([[pb stringForType:@"dev.remote-native.clipboard-origin"] isEqualToString:@"probe-origin"],"native origin marker present");
    before=pb.changeCount; ClipboardPacket invalid{43,ClipboardKind::Text,{0xff,0xfe}};
    Check(!RemoteMacInstallClipboard(pb,invalid,@"bad") && pb.changeCount==before,"invalid UTF-8 preserves clipboard");
    invalid.kind=ClipboardKind::Png;
    Check(!RemoteMacInstallClipboard(pb,invalid,@"bad") && pb.changeCount==before,"invalid PNG preserves clipboard");
    NSBitmapImageRep *bitmap=[[NSBitmapImageRep alloc] initWithBitmapDataPlanes:nullptr pixelsWide:8 pixelsHigh:5
        bitsPerSample:8 samplesPerPixel:4 hasAlpha:YES isPlanar:NO colorSpaceName:NSDeviceRGBColorSpace bytesPerRow:32 bitsPerPixel:32];
    for(int i=0;i<40;++i) { bitmap.bitmapData[i*4]=80;bitmap.bitmapData[i*4+1]=160;bitmap.bitmapData[i*4+2]=240;bitmap.bitmapData[i*4+3]=i?127:0; }
    NSData *png=[bitmap representationUsingType:NSBitmapImageFileTypePNG properties:@{}];
    const uint8_t *image=(const uint8_t *)png.bytes; packet={44,ClipboardKind::Png,std::vector<uint8_t>(image,image+png.length)};
    Check(RemoteMacInstallClipboard(pb,packet,@"probe-origin"),"install lossless PNG");
    NSBitmapImageRep *roundtrip=[NSBitmapImageRep imageRepWithData:[pb dataForType:NSPasteboardTypePNG]];
    Check(roundtrip.pixelsWide==8 && roundtrip.pixelsHigh==5 && roundtrip.hasAlpha,"PNG dimensions and alpha preserved");
    NSUInteger pixel[4]={}; [roundtrip getPixel:pixel atX:0 y:0]; Check(pixel[3]==0,"PNG transparent pixel preserved");
    NSString *fixture=[NSTemporaryDirectory() stringByAppendingPathComponent:[@"remote-clipboard-probe-" stringByAppendingString:NSUUID.UUID.UUIDString]];
    fs::path root(fixture.fileSystemRepresentation); fs::create_directories(root/"source"/"子目录");
    std::ofstream(root/"source"/"文件.txt",std::ios::binary)<<"synthetic fixture only";
    std::vector<fs::path> files={root/"source"/"文件.txt",root/"source"/"子目录"};
    Check(RemoteMacInstallFiles(pb,files,@"probe-origin"),"install real file URLs");
    auto restored=RemoteMacClipboardFiles(pb);
    Check(restored==files,"file and folder URLs survive origin tagging");
    Check([[unrelated stringForType:NSPasteboardTypeString] isEqualToString:@"sentinel"],"unrelated private pasteboard untouched");
    Session *aPtr=nullptr,*bPtr=nullptr;
    Session a(true,root/"mac-receive",[&](const uint8_t *data,size_t size){return bPtr&&bPtr->Receive(data,size)?1:0;});aPtr=&a;
    Session b(false,root/"win-receive",[&](const uint8_t *data,size_t size){return aPtr&&aPtr->Receive(data,size)?1:0;});bPtr=&b;
    int pastes=0;
    RemoteMacExchange native(a,[&](uint32_t){++pastes;},pb,[fixture stringByAppendingPathComponent:@"cache"]);
    a.SetConnected(true);b.SetConnected(true);
    Check(Wait([&]{return a.GetSnapshot().connected&&b.GetSnapshot().connected;},&native),"native exchange session connected");
    ClipboardPacket received;
    Check(!b.TakeClipboard(received),"session start does not upload existing clipboard");
    uint64_t id=b.PublishClipboard(ClipboardKind::Text,Bytes(sample));
    Check(Wait([&]{return [[pb stringForType:NSPasteboardTypeString] isEqualToString:sample];},&native),"remote text installed through production adapter");
    Check(Wait([&]{return b.GetSnapshot().clipboardAcknowledged==id;},&native),"installation acknowledgement reaches publisher");
    native.RequestPaste(id,2);
    Check(pastes==1,"prepared paste invokes one atomic native callback");
    native.RequestPaste(id+100,2);
    [pb clearContents];[pb setString:@"new clipboard cancels pending paste" forType:NSPasteboardTypeString];
    std::this_thread::sleep_for(std::chrono::milliseconds(40));native.Tick();
    Check(pastes==1,"new local clipboard cancels stale pending paste");
    bool cancelledCopy=Wait([&]{return b.TakeClipboard(received);},&native);
    Check(cancelledCopy,"new local copy still synchronizes after cancelled paste");
    if(cancelledCopy)b.ClipboardInstalled(received.id,true);
    std::this_thread::sleep_for(std::chrono::milliseconds(150));native.Tick();
    Check(!b.TakeClipboard(received),"remote clipboard write does not echo");
    [pb clearContents];[pb setString:@"本机新复制 💻" forType:NSPasteboardTypeString];
    bool local=Wait([&]{return b.TakeClipboard(received);},&native);
    Check(local && received.kind==ClipboardKind::Text && received.data==Bytes(@"本机新复制 💻"),"native local copy synchronizes once");
    if(local)b.ClipboardInstalled(received.id,true);
    NSData *tiff=[bitmap representationUsingType:NSBitmapImageFileTypeTIFF properties:@{}];
    [pb clearContents];[pb setData:tiff forType:NSPasteboardTypeTIFF];
    bool converted=Wait([&]{return b.TakeClipboard(received);},&native);
    NSData *convertedData=[NSData dataWithBytes:received.data.data() length:received.data.size()];
    NSBitmapImageRep *convertedImage=[NSBitmapImageRep imageRepWithData:convertedData];
    Check(converted && received.kind==ClipboardKind::Png && convertedImage.pixelsWide==8 && convertedImage.hasAlpha,"native TIFF clipboard converts to lossless PNG off UI thread");
    if(converted)b.ClipboardInstalled(received.id,true);
    NSString *sourcePath=[[NSString alloc] initWithUTF8String:(root/"source"/"文件.txt").string().c_str()];
    [pb clearContents];[pb writeObjects:@[[NSURL fileURLWithPath:sourcePath]]];
    // The private fixture publishes an offer only; copying never sends file bytes.
    Check(Wait([&]{return b.GetSnapshot().remoteOffer.id!=0;},&native),"native file copy publishes metadata offer");
    Check(b.GetSnapshot().transfers.empty(),"copying file does not begin transfer");
    uint64_t transfer=b.RequestFiles(b.GetSnapshot().remoteOffer.id,root/"download");
    CompletedFiles complete;
    Check(Wait([&]{return b.TakeCompletedFiles(complete);},&native,10000) && complete.transferId==transfer,"offered file transfers on demand");
    Check(fs::exists(root/"source"/"文件.txt"),"source file remains after transfer");
    uint64_t offered=b.OfferFiles(files);
    Check(Wait([&]{return a.GetSnapshot().remoteOffer.id==offered;},&native),"Windows-side file offer arrives at native Mac adapter");
    native.RequestPaste(offered,2);
    Check(Wait([&]{return pastes==2;},&native,10000),"file paste waits for cache then invokes native callback");
    auto cached=RemoteMacClipboardFiles(pb);
    Check(cached.size()==2 && cached[0].string().find("cache")!=std::string::npos,"prepared Finder paste contains real cached file URLs");
    Check(Wait([&]{return b.GetSnapshot().clipboardAcknowledged==offered;},&native),"file readiness acknowledgement matches exact offer ID");
    a.SetConnected(false);b.SetConnected(false);
    Wait([&]{return !a.GetSnapshot().connected;},&native);native.Tick();
    [pb clearContents];[pb setString:@"断线时复制" forType:NSPasteboardTypeString];
    std::this_thread::sleep_for(std::chrono::milliseconds(150));native.Tick();
    Check(!b.TakeClipboard(received),"disconnected clipboard is not exported");
    a.SetConnected(true);b.SetConnected(true);Wait([&]{return a.GetSnapshot().connected;},&native);native.Tick();
    std::this_thread::sleep_for(std::chrono::milliseconds(150));native.Tick();
    Check(!b.TakeClipboard(received),"reconnect does not upload old disconnected copy");
    Check(RemoteHostRenderPreview(@"dist/probes/mac-sharing-ui.png"),"native Chinese sharing UI preview renders");
    Check(RemoteMacRenderTransfersPreview(a,@"dist/probes/mac-file-transfers-ui.png"),"native transfer window preview renders");
    fs::remove_all(root);[pb releaseGlobally];[unrelated releaseGlobally];
    std::cout<<"RESULT "<<passed<<" passed, "<<failed<<" failed\n";
    return failed?1:0;
} }
