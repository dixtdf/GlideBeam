#import "../login-startup.hpp"
int main(void) {
    @autoreleasepool {
        (void)&RemoteLoginEnabled;
        (void)&RemoteLoginSetEnabled;
        return RemoteLoginSelfTest();
    }
}
