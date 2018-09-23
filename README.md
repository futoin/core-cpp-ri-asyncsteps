
### FutoIn AsyncSteps reference C++ implementation

See [**FTN12: AsyncSteps**](https://futoin.org/docs/asyncsteps/) for more details.

### Usage

Please refer to FutoIn/Core/Native C++ API for details of AsyncSteps interface.

#### Basics

- `futoin::ri::AsyncTool` is implementation of `futoin::IAsyncTool` event loop interface.
- `futoin::ri::AsyncSteps` is implementation of `futoin::IAsyncSteps` FTN12 interface
- `futoin::ri::NitroSteps` is alternative performance-focused implementation
- there are the following FTN12 synchronization primitives:
    - `futoin::ri::Mutex` and `futoin::ri::ThreadlessMutex`
    - `futoin::ri::Throttle` and `futoin::ri::ThreadlessThrottle`
    - `futoin::ri::Limiter` and `futoin::ri::ThreadlessLimiter`

#### AsyncSteps

```c++
#include <futoin/ri/asyncsteps.hpp>
#include <futoin/ri/asynctool.hpp>

void inner_thread() {
    futoin::ri::AsyncTool at;
    
    futoin::ri::AsyncSteps asi;
    asi.state("requests", RequestManager());

    asi.loop([&at](futoin::IAsyncSteps &asi){
        // Some infinite loop logic
        auto request = ...;
        
        // Handle some new request
        auto steps = asi.newInstance().release();
        
        // That's just for example, real implementation must
        // manage request objects (their std::unique_ptr references).
        auto cleanup = [&at,steps]() {
            at.immediate([steps](){
                delete steps;
            });
        };
        
        steps->add([cleanup, request](futoin::IAsyncSteps &asi){
            asi.setCancel([cleanup](futoin::IAsyncSteps &asi){
                cleanup();
            });
            call_business_logic(asi, request);
        });
        steps->add([cleanup](futoin::IAsyncSteps &asi){
            cleanup();
        });
        steps->execute();
    });
    
    asi.promise.wait();
}

void external_event_loop() {
    futoin::ri::AsyncTool::Params prm;
    prm.mempool_mutex = false; // boost performance for single threaded

    futoin::ri::AsyncTool at([](){
        // Called when new jobs are scheduled through
        // immediate() or deferred() API from other threads.
        //
        // It's never called, if AsyncTool is not exposed to other threads.
        //
        // This callback disables spawn of internal thread.
    }, prm);

    for (;;) {
        // Real implementation should call it not earlier than delay
        // and only if there is some work to do.
        auto res = at.iterate();
        
        if (!res.have_work) {
            // wait for external events
        } else if (res.delay > 0) {
            // wait for external events with specified delay
        }
    }
}
```

#### NitroSteps

`NitroSteps` is implemented as template with all internals in pre-allocated buffers with
only exception for parallel() sub-steps.

It general, case-optimized `NitroSteps` may perform better than the default `AsyncSteps`, but
there are edge cases where it perform worse. So, `futoin::ri::AsyncSteps` is safe option
unless case-specific optimization is done.

```c++
#include <futoin/ri/asynctool.hpp>
#include <futoin/ri/nitrosteps.hpp>

void inner_thread() {
    futoin::ri::AsyncTool at;
    
    futoin::ri::NitroSteps<> asi_default{at};
    // Uses the defaults:
    // futoin::ri::nitro::MaxSteps<16>
    // futoin::ri::nitro::MaxTimeouts<4>
    // futoin::ri::nitro::MaxCancels<4>
    // futoin::ri::nitro::MaxExtended<4>
    // futoin::ri::nitro::MaxStackAllocs<8>
    // futoin::ri::nitro::ErrorCodeMaxSize<32>

    futoin::ri::NitroSteps<
        futoin::ri::nitro::MaxSteps<8>
        futoin::ri::nitro::MaxExtended<1>
    > asi_custom_example{at};
    
    asi_default.add([](futoin::IAsyncSteps &asi){
        // ...
    });
    asi_default.execute();
}

#### Mutex

Mutex is used to limit concurrent execution of AsyncSteps flows.
The Mutex is recursive.

A strictly ordered queue of pending flows is supported. If queue limit
is reached then `DefenseRejected` error is raised.

```c++
#include <futoin/ri/mutex.hpp>

using futoin::ri::Mutex;

// 1 concurrent flow with infinite wait queue
Mutex mtx_a;

// 10 concurrent flows with infinite wait queue
Mutex mtx_b{10};

// 1 concurrent flow with queue of 8 pending flows
Mutex mtx_c{1, 8};

asi.sync(mtx_a, [](IAsyncSteps& asi) {
    // synchronized section
    asi.add([](IAsyncSteps& asi) {
        // inner step in the section
        
        // This synchronization is NOOP for already
        // acquired Mutex.
        asi.sync(mtx_a, [](IAsyncSteps& asi) {
        });
    });
});
```

#### Throttle

Throttle is used to limit number of flows which pass the this barrier
in period of time.

Pending queue is supported as for the `Mutex`.

```c++
#include <futoin/ri/throttle.hpp>

// Required to schedule period reset timer
futoin::ri::AsyncTool at;

using futoin::ri::Throttle;

// 2 flows-per-second with infinite queue
Throttle thr_a(at, 2);

// 4 flows-per-15-seconds with infinite queue
Throttle thr_b(at, 4, std::chrono::seconds(15));

// 4 flows-per-500-milliseconds with queue size of 12 flows
Throttle thr_c(at, 4, std::chrono::milliseconds(500), 12);

asi.sync(thr_a, [](IAsyncSteps& asi) {
    // synchronized section after rate barrier
    
    // This synchronization is accounted in rate!
    asi.sync(thr_a, [](IAsyncSteps& asi) {
    });
});
```

#### Limiter

It's combination of `Mutex` and `Throttle`. It's typically used
to limit incoming and outgoing requests to evade attacks and
avoid accidental self-DoS.


```c++
#include <futoin/ri/limiter.hpp>

// Required to schedule period reset timer
futoin::ri::AsyncTool at;

using futoin::ri::Limiter;

// 1 concurrent flow with infinite wait queue
// 2 flows-per-second with infinite queue
Limiter::Params prm_a;
prm_a.rate = 2;

Limiter lmtr_a(at, prm_a);

// 10 concurrent flows with infinite wait queue
// 4 flows-per-15-seconds with infinite queue
Limiter::Params prm_b;
prm_b.concurrency = 10;
prm_b.rate = 4;
prm_b.period = std::chrono::seconds(15);

Limiter lmtr_b(at, prm_b);

// 1 concurrent flow with queue of 8 pending flows
// 4 flows-per-500-milliseconds with queue size of 12 flows
Limiter::Params prm_c;
prm_c.concurrency = 1;
prm_c.max_queue = 8;
prm_c.rate = 4;
prm_c.period = std::chrono::milliseconds(500);
prm_c.burst = 12;

Limiter lmtr_c(at, 4, std::chrono::milliseconds(500), 12);

asi.sync(lmtr_a, [](IAsyncSteps& asi) {
    // synchronized section after rate barrier
    
    // Not accounted for concurrency, but accounted for rate!
    asi.sync(lmtr_a, [](IAsyncSteps& asi) {
    });
});
```
