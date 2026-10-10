#import "localization.h"
#pragma once
#import <Foundation/Foundation.h>
#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>
#include <cerrno>
#include <cstdio>

// A per-user Aqua LaunchAgent starts this app only after login. No administrator
// privileges, network credentials, or permission prompts are stored in its plist.
static NSString *const RemoteLoginLabel = @"dev.remote-native.host.login";
static NSString *RemoteLoginPath(void) {
    return [[NSHomeDirectory() stringByAppendingPathComponent:@"Library/LaunchAgents"]
        stringByAppendingPathComponent:[RemoteLoginLabel stringByAppendingString:@".plist"]];
}
static NSDictionary *RemoteLoginConfiguration(NSString *executable, NSString *support) {
    NSString *log = [support stringByAppendingPathComponent:@"agent.log"];
    return @{@"Label":RemoteLoginLabel, @"ProgramArguments":@[executable, @"--background"],
        @"RunAtLoad":@YES, @"LimitLoadToSessionType":@"Aqua", @"ProcessType":@"Interactive",
        @"KeepAlive":@{@"SuccessfulExit":@NO}, @"ThrottleInterval":@10,
        @"StandardOutPath":log, @"StandardErrorPath":log, @"Umask":@077};
}
static BOOL RemoteLoginEnabled(void) {
    NSData *data = [NSData dataWithContentsOfFile:RemoteLoginPath()];
    id decoded = data ? [NSPropertyListSerialization propertyListWithData:data options:NSPropertyListImmutable format:nil error:nil] : nil;
    if (![decoded isKindOfClass:NSDictionary.class]) return NO;
    NSDictionary *value = decoded;
    NSArray *arguments = value[@"ProgramArguments"];
    return [value[@"Label"] isEqualToString:RemoteLoginLabel] && [value[@"RunAtLoad"] boolValue] &&
        [arguments isKindOfClass:NSArray.class] && arguments.count == 2 &&
        [arguments[0] isEqualToString:NSBundle.mainBundle.executablePath] && [arguments[1] isEqualToString:@"--background"];
}
static BOOL RemoteLoginSetEnabled(BOOL enabled, NSString **message) {
    NSString *path = RemoteLoginPath();
    NSFileManager *files = NSFileManager.defaultManager;
    NSError *error = nil;
    NSData *previous = [NSData dataWithContentsOfFile:path];
    BOOL existed = [files fileExistsAtPath:path];
    if (existed && !previous) { if (message) *message = RemoteHostText(@"无法读取原自启配置，未做修改。"); return NO; }
    auto restore = [&] {
        if (previous) { [previous writeToFile:path options:NSDataWritingAtomic error:nil]; [files setAttributes:@{NSFilePosixPermissions:@0600} ofItemAtPath:path error:nil]; }
        else if ([files fileExistsAtPath:path]) [files removeItemAtPath:path error:nil];
    };
    if (enabled) {
        if (![files createDirectoryAtPath:path.stringByDeletingLastPathComponent withIntermediateDirectories:YES
            attributes:@{NSFilePosixPermissions:@0700} error:&error]) {
            if (message) *message = RemoteHostText(@"无法创建登录自启目录，请检查本机文件权限。"); return NO;
        }
        NSString *support = [[NSSearchPathForDirectoriesInDomains(NSApplicationSupportDirectory, NSUserDomainMask, YES) firstObject]
            stringByAppendingPathComponent:@"RemoteNative"];
        NSDictionary *configuration = RemoteLoginConfiguration(NSBundle.mainBundle.executablePath, support);
        NSData *plist = [NSPropertyListSerialization dataWithPropertyList:configuration format:NSPropertyListXMLFormat_v1_0 options:0 error:&error];
        if (!plist || ![plist writeToFile:path options:NSDataWritingAtomic error:&error] ||
            ![files setAttributes:@{NSFilePosixPermissions:@0600} ofItemAtPath:path error:&error]) {
            restore();
            if (message) *message = RemoteHostText(@"无法保存登录自启配置，请检查本机文件权限。"); return NO;
        }
    } else if (existed && ![files removeItemAtPath:path error:&error]) {
        if (message) *message = RemoteHostText(@"无法移除登录自启配置，请检查本机文件权限。"); return NO;
    }
    // Enabling takes effect at the next login; the current app already runs.
    // Disabling future launches does not terminate an active remote connection.
    NSTask *task = [NSTask new];
    task.executableURL = [NSURL fileURLWithPath:@"/bin/launchctl"];
    task.arguments = @[enabled ? @"enable" : @"disable", [NSString stringWithFormat:@"gui/%u/%@", getuid(), RemoteLoginLabel]];
    task.standardOutput = NSFileHandle.fileHandleWithNullDevice;
    task.standardError = NSFileHandle.fileHandleWithNullDevice;
    if (![task launchAndReturnError:&error]) {
        restore();
        if (message) *message = RemoteHostText(@"无法更新当前用户的自启状态，已尝试恢复原配置。"); return NO;
    }
    [task waitUntilExit];
    if (task.terminationStatus != 0) {
        restore();
        if (message) *message = RemoteHostText(@"系统未接受自启设置，已尝试恢复原配置。"); return NO;
    }
    return YES;
}
static int RemoteLoginSelfTest(void) {
    NSFileManager *files=NSFileManager.defaultManager;
    NSString *directory=[NSTemporaryDirectory() stringByAppendingPathComponent:[@"remote-login-test-" stringByAppendingString:NSUUID.UUID.UUIDString]];
    BOOL created=[files createDirectoryAtPath:directory withIntermediateDirectories:NO attributes:@{NSFilePosixPermissions:@0700} error:nil];
    NSDictionary *configuration=RemoteLoginConfiguration(@"/Users/Shared/RemoteNative/RemoteHost.app/Contents/MacOS/RemoteHost",directory);
    NSData *serialized=[NSPropertyListSerialization dataWithPropertyList:configuration format:NSPropertyListXMLFormat_v1_0 options:0 error:nil];
    NSDictionary *decoded=serialized ? [NSPropertyListSerialization propertyListWithData:serialized options:NSPropertyListImmutable format:nil error:nil] : nil;
    BOOL roundtrip=decoded && [decoded isEqualToDictionary:configuration];
    BOOL aqua=[decoded[@"LimitLoadToSessionType"] isEqualToString:@"Aqua"];
    BOOL background=[decoded[@"ProgramArguments"] isEqualToArray:@[@"/Users/Shared/RemoteNative/RemoteHost.app/Contents/MacOS/RemoteHost",@"--background"]];
    BOOL restarts=[decoded[@"RunAtLoad"] boolValue] && ![decoded[@"KeepAlive"][@"SuccessfulExit"] boolValue] && [decoded[@"ThrottleInterval"] intValue]==10;
    BOOL noSecrets=configuration.count==10 && !configuration[@"EnvironmentVariables"] && !configuration[@"UserName"];
    NSString *lock=[directory stringByAppendingPathComponent:@"instance.lock"];
    int first=created ? open(lock.fileSystemRepresentation,O_CREAT|O_RDWR|O_CLOEXEC,0600) : -1;
    BOOL acquired=first>=0 && flock(first,LOCK_EX|LOCK_NB)==0;
    struct stat mode{};BOOL privateMode=first>=0 && fstat(first,&mode)==0 && (mode.st_mode&0777)==0600;
    int second=created ? open(lock.fileSystemRepresentation,O_RDWR|O_CLOEXEC) : -1;
    BOOL duplicate=second>=0 && flock(second,LOCK_EX|LOCK_NB)!=0 && (errno==EWOULDBLOCK||errno==EAGAIN);
    if(first>=0)close(first);
    BOOL released=second>=0 && flock(second,LOCK_EX|LOCK_NB)==0;
    if(second>=0)close(second);
    if(created)[files removeItemAtPath:directory error:nil];
    BOOL cleanup=created && ![files fileExistsAtPath:directory];
    BOOL pass=roundtrip&&aqua&&background&&restarts&&noSecrets&&acquired&&privateMode&&duplicate&&released&&cleanup;
    NSDictionary *report=@{@"schema":@"remote.login-startup-test.v1",@"configuration_roundtrip":@(roundtrip),@"gui_login_only":@(aqua),@"background_arguments":@(background),@"restart_failure_only":@(restarts),@"no_credentials_in_job":@(noSecrets),@"single_instance_lock":@(acquired&&duplicate),@"private_lock_mode":@(privateMode),@"lock_released_after_exit":@(released),@"temporary_cleanup":@(cleanup),@"pass":@(pass)};
    NSData *json=[NSJSONSerialization dataWithJSONObject:report options:NSJSONWritingPrettyPrinted error:nil];
    if(json)fwrite(json.bytes,1,json.length,stdout);fputc('\n',stdout);
    return pass ? 0 : 6;
}
