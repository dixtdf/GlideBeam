#include "../encoder-wake.h"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <future>
#include <mutex>
#include <thread>

using namespace std::chrono_literals;
static bool Await(std::future<bool>& result,EncoderWakeSignal& wake,bool expected) {
    if(result.wait_for(2s)!=std::future_status::ready) { wake.Stop(); result.wait(); return false; }
    return result.get()==expected;
}
int main() {
    unsigned passed=0,failed=0;
    auto check=[&](const char* name,bool value) {
        std::printf("%s %s\n",value ? "PASS" : "FAIL",name);
        value ? ++passed : ++failed;
    };
    {
        EncoderWakeSignal wake;
        auto observed=wake.Observe();
        wake.Notify(); // Publish after check, before entering wait.
        auto result=std::async(std::launch::async,[&]{return wake.Wait(observed);});
        check("notification_before_wait_is_not_lost",Await(result,wake,true));
    }
    {
        EncoderWakeSignal wake;
        auto observed=wake.Observe();
        std::promise<void> entered;
        auto result=std::async(std::launch::async,[&]{entered.set_value();return wake.Wait(observed);});
        entered.get_future().wait();
        check("idle_worker_blocks_without_polling",result.wait_for(20ms)==std::future_status::timeout);
        wake.Notify();
        check("publication_wakes_blocked_worker",Await(result,wake,true));
    }
    {
        EncoderWakeSignal wake;
        auto observed=wake.Observe();
        auto result=std::async(std::launch::async,[&]{return wake.Wait(observed);});
        wake.Stop();
        check("stop_wakes_worker_and_requests_exit",Await(result,wake,false));
        check("wait_after_stop_also_exits",!wake.Wait(wake.Observe()));
    }
    {
        // Stress the production snapshot/check/wait ordering and a bounded
        // latest-only mailbox while the consumer is held by a single flight.
        EncoderWakeSignal wake;
        std::mutex stateMutex;
        std::condition_variable acknowledged;
        unsigned latest=0,consumed=0,inflight=0,maxInflight=0;
        std::atomic<bool> running{true};
        auto worker=std::async(std::launch::async,[&] {
            while(running.load()) {
                auto observed=wake.Observe();
                bool worked=false;
                { std::lock_guard lock(stateMutex);
                  if(!inflight && latest!=consumed) {
                      consumed=latest;inflight=1;maxInflight=std::max(maxInflight,inflight);worked=true;
                  } }
                if(worked)acknowledged.notify_all();
                else if(!wake.Wait(observed))break;
            }
            return true;
        });
        bool complete=true;
        for(unsigned n=1;n<=2000 && complete;++n) {
            {
                std::lock_guard lock(stateMutex);
                latest=n*3;
                inflight=0; // Prior callback completion opens the flight gate.
            }
            wake.Notify(); // Also models a synchronous callback before wait.
            {
                std::unique_lock lock(stateMutex);
                complete=acknowledged.wait_for(lock,2s,[&]{return consumed==n*3 && inflight==1;});
                if(complete)latest=n*3+1; // Publish while encoding is busy.
            }
            wake.Notify();
            // The next iteration replaces this pending frame before the gate
            // opens. It must never create a second encoder submission.
        }
        running.store(false);wake.Stop();
        check("publish_callback_races_keep_single_latest_flight",Await(worker,wake,true)&&complete&&maxInflight==1&&consumed==6000);
    }
    std::printf("encoder-wake: %u passed, %u failed\n",passed,failed);
    return failed ? 1 : 0;
}
