// Standalone probe; no network, global timer-resolution changes or busy loops.
#include "../worker-wait.h"
#include "../../../shared/latency-stats.h"
#include <iostream>
#include <stdexcept>

namespace {
void Check(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}
struct FakeState {
    bool createOk = true, setOk = true, stopDuringWait = false;
    DWORD waitResult = WAIT_OBJECT_0;
    uint32_t creates = 0, sets = 0, waits = 0, cancels = 0, closes = 0, sleeps = 0;
    DWORD flags = 0, access = 0, waitTimeout = 0, sleepMs = 0, tolerableDelay = 1;
    LONGLONG due = 0;
    LONG period = -1;
    bool unnamed = false, noApc = false, noWakeContext = false;
    std::atomic<bool>* stop = nullptr;
};
FakeState* fake = nullptr;
HANDLE FakeHandle() { return reinterpret_cast<HANDLE>(static_cast<uintptr_t>(0x1234)); }
HANDLE WINAPI Create(LPSECURITY_ATTRIBUTES attributes, LPCWSTR name, DWORD flags, DWORD access) {
    ++fake->creates;fake->unnamed=!attributes&&!name;fake->flags=flags;fake->access=access;
    return fake->createOk?FakeHandle():nullptr;
}
BOOL WINAPI Set(HANDLE timer, const LARGE_INTEGER* due, LONG period, PTIMERAPCROUTINE callback,
                LPVOID argument, PREASON_CONTEXT context, ULONG tolerableDelay) {
    Check(timer==FakeHandle(),"wrong timer set");++fake->sets;fake->due=due->QuadPart;fake->period=period;
    fake->noApc=!callback&&!argument;fake->noWakeContext=!context;fake->tolerableDelay=tolerableDelay;
    return fake->setOk?TRUE:FALSE;
}
DWORD WINAPI Wait(HANDLE timer,DWORD milliseconds) {
    Check(timer==FakeHandle(),"wrong timer wait");++fake->waits;fake->waitTimeout=milliseconds;
    if(fake->stopDuringWait&&fake->stop)fake->stop->store(true);
    return fake->waitResult;
}
BOOL WINAPI Cancel(HANDLE timer) { Check(timer==FakeHandle(),"wrong timer cancellation");++fake->cancels;return TRUE; }
BOOL WINAPI Close(HANDLE timer) { Check(timer==FakeHandle(),"wrong timer close");++fake->closes;return TRUE; }
void Sleep(DWORD milliseconds) { ++fake->sleeps;fake->sleepMs=milliseconds; }
remote_video_worker::WaitFunctions Functions(FakeState& state) {
    fake=&state;remote_video_worker::WaitFunctions result;
    result.create=&Create;result.set=&Set;result.wait=&Wait;result.cancel=&Cancel;result.close=&Close;result.sleep=&Sleep;
    return result;
}
void FailureAndLifetimeChecks() {
    using remote_video_worker::WorkerWait;
    std::atomic<bool> stop{false};
    FakeState success;
    {
        WorkerWait worker(Functions(success));worker.Wait(stop);worker.Wait(stop);
        Check(worker.HighResolution()&&worker.FallbackWaits()==0&&worker.Failures()==0,"successful timer fell back");
        Check(success.creates==1&&success.sets==2&&success.waits==2,"timer not reused");
        Check(success.unnamed&&success.flags==CREATE_WAITABLE_TIMER_HIGH_RESOLUTION&&
              success.access==(TIMER_MODIFY_STATE|SYNCHRONIZE),"timer creation flags/access incorrect");
        Check(success.due==-10000&&success.period==0&&success.tolerableDelay==0&&success.noApc&&success.noWakeContext,
              "wait not one-shot relative 1 ms");
        Check(success.waitTimeout==WorkerWait::MaximumWaitMs&&success.waitTimeout!=INFINITE,"timer wait unbounded");
    }
    Check(success.cancels==1&&success.closes==1,"timer lifetime leak/double close");
    FakeState unavailable;unavailable.createOk=false;
    {
        WorkerWait worker(Functions(unavailable));worker.Wait(stop);worker.Wait(stop);
        Check(!worker.HighResolution()&&worker.FallbackWaits()==2&&worker.Failures()==1,"create failure fallback missing");
    }
    Check(unavailable.creates==1&&unavailable.sets==0&&unavailable.waits==0&&unavailable.sleeps==2&&
          unavailable.sleepMs==1&&unavailable.closes==0,"invalid handle used or fallback changed");
    const DWORD waitResults[]={WAIT_OBJECT_0,WAIT_FAILED,WAIT_TIMEOUT};
    for(DWORD result:waitResults) {
        FakeState failure;failure.setOk=result!=WAIT_OBJECT_0;failure.waitResult=result;
        {
            WorkerWait worker(Functions(failure));worker.Wait(stop);worker.Wait(stop);
            Check(!worker.HighResolution()&&worker.Failures()==1&&worker.FallbackWaits()==2,"failed timer not disabled");
            Check(failure.sleeps==2&&failure.sleepMs==1&&failure.cancels==1&&failure.closes==1,"failed timer cleanup/fallback wrong");
            Check(failure.sets==1&&failure.waits==(failure.setOk?1u:0u),"timer retried after failure");
        }
        Check(failure.closes==1,"failed timer double closed");
    }
    FakeState stopped;stop=true;
    {
        WorkerWait worker(Functions(stopped));worker.Wait(stop);
        Check(stopped.sets==0&&stopped.waits==0&&stopped.sleeps==0,"stopped worker still waited");
    }
    Check(stopped.closes==1,"stopped worker leaked handle");
    stop=false;FakeState race;race.waitResult=WAIT_TIMEOUT;race.stopDuringWait=true;race.stop=&stop;
    {
        WorkerWait worker(Functions(race));worker.Wait(stop);worker.Wait(stop);
        Check(stop&&race.waits==1&&race.sleeps==0,"shutdown during timer failure still slept");
        Check(race.waitTimeout==WorkerWait::MaximumWaitMs&&race.closes==1,"shutdown wait/cleanup unbounded");
    }
}
HANDLE realCreated=nullptr;
HANDLE WINAPI RecordCreate(LPSECURITY_ATTRIBUTES attributes,LPCWSTR name,DWORD flags,DWORD access) {
    realCreated=CreateWaitableTimerExW(attributes,name,flags,access);return realCreated;
}
void RealWaitChecks() {
    remote_video_worker::WaitFunctions functions;functions.create=&RecordCreate;
    std::atomic<bool> stop{false};RollingLatencyStats timing;bool highResolution=false;uint64_t fallback=0;
    {
        remote_video_worker::WorkerWait worker(functions);
        for(unsigned i=0;i<16;++i) {
            auto start=std::chrono::steady_clock::now();worker.Wait(stop);
            timing.ObserveUs(static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(
                std::chrono::steady_clock::now()-start).count()));
        }
        highResolution=worker.HighResolution();fallback=worker.FallbackWaits();
        stop=true;worker.Wait(stop);
        Check(worker.FallbackWaits()==fallback,"real stopped worker fell back");
    }
    if(realCreated) {
        DWORD flags=0;SetLastError(ERROR_SUCCESS);
        Check(!GetHandleInformation(realCreated,&flags)&&GetLastError()==ERROR_INVALID_HANDLE,"real timer handle not closed");
    }
    const auto measured=timing.Snapshot();
    // Record real scheduling behavior without fragile performance assertions.
    std::cout<<"{\"real_timer_supported\":"<<(highResolution?"true":"false")<<",\"samples\":"<<measured.totalSamples
             <<",\"fallback_waits\":"<<fallback<<",\"mean_ms\":"<<measured.meanUs/1000.0
             <<",\"p95_ms\":"<<measured.p95Us/1000.0<<",\"p99_ms\":"<<measured.p99Us/1000.0<<"}\n";
}
} // namespace
int main() {
    try { FailureAndLifetimeChecks();RealWaitChecks();std::cout<<"PASS worker-wait failure/stop/lifetime checks\n";return 0; }
    catch(const std::exception& error) { std::cerr<<"FAIL worker-wait: "<<error.what()<<'\n';return 1; }
}
