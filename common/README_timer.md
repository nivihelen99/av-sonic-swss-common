# Timer Manager Library (`TimerMgr`)

## Overview

The `TimerMgr` library provides an efficient timer mechanism for applications requiring multiple timers with minimal resource overhead. Instead of using one system `timer_fd` per timer, `TimerMgr` utilizes a single `timer_fd` for all managed timers. Timer processing and callback invocation are driven by periodic external calls to the `processTick()` method.

**Key Features:**

*   **Configurable Base Interval:** Timer durations are multiples of a chosen base interval (e.g., 10ms, 100ms), defining the timer granularity.
*   **External Tick Processing:** Relies on the user to call `processTick()` to advance timers and execute callbacks.
*   **One-Shot and Cyclic Timers:** Supports both timers that fire once and timers that fire repeatedly.
*   **User-Provided Context:** Allows arbitrary `void*` data (cookie) to be passed to timer callbacks.
*   **Thread-Safety:** Public API methods (`createTimer`, etc.) and `processTick()` are thread-safe when called from different threads.

**Problem Solved:**
This library addresses the inefficiency and resource consumption issues that arise when managing many timers by creating a separate `timer_fd` (or equivalent system resource) for each individual timer. By multiplexing all timers onto a single `timer_fd` and using an external tick mechanism, it offers flexibility in integrating with various application event loops.

## Design

### Core Idea
The library's core design revolves around a single POSIX timer file descriptor (`m_timerFd`) created using `timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK)`. Timer state advancement and callback execution are driven by external calls to the `processTick()` method.

### Base Interval
All timer durations must be non-zero multiples of a configurable base interval. This interval is set when the `TimerMgr` instance is created and determines the fundamental "tick" rate and granularity of the timers. Available base intervals are: 10ms, 100ms, 250ms, 500ms, and 1 second.

### Timer Tick and `processTick()`
The library internally operates on discrete "ticks" (`m_currentTick`). Each call to the public `processTick()` method increments `m_currentTick` by one, representing the passage of one base interval.
The user (or an external system) is responsible for calling `processTick()` periodically, ideally at a frequency matching the `BaseInterval` the `TimerMgr` was configured with. This call:
1.  Advances the internal `m_currentTick`.
2.  Checks `m_activeTimers` for any timers whose `expirationTick` is less than or equal to the new `m_currentTick`.
3.  Collects callbacks for these expired timers.
4.  Reschedules cyclic timers or marks one-shot timers as inactive.
5.  Calls `updateTimerFd()` to re-arm `m_timerFd` for the next actual timer event.
6.  Executes the collected callbacks (see Callback Execution section).

### Data Structures
*   `std::map<int64_t, TimerInfo> m_timers`: This map stores detailed information for every timer, including its callback, cookie, interval, type (one-shot/cyclic), and current state. Timers are uniquely identified by an `int64_t` ID.
*   `std::priority_queue<std::pair<uint64_t, int64_t>> m_activeTimers`: This priority queue stores pairs of `(expirationTick, timerId)`. It is ordered by `expirationTick`, ensuring that the timer that will expire next is always at the top. This allows for O(1) access to the next due timer.

### Thread Model and `m_timerFd` Usage
`TimerMgr` no longer uses an internal thread for timing or callback execution. This responsibility is shifted to the caller of `processTick()`.

*   **External Tick Source:** The application must have an external mechanism (e.g., its own event loop, a dedicated ticker thread) that calls `processTick()` at regular intervals (recommended to be the `BaseInterval`).
*   **`m_timerFd` Role:** The internal `m_timerFd` is still used by `TimerMgr`. The `updateTimerFd()` method (called by `processTick()` and API methods) arms this `timer_fd` to signal when the *next closest application timer* is due to expire.
    *   When `processTick()` is called, it performs a non-blocking `read()` on `m_timerFd`. This primarily serves to clear the timer event from the kernel if it has fired. The actual advancement of `m_currentTick` and processing of application timers is driven by the `processTick()` call itself, not directly by the value read from `m_timerFd`.
    *   An external system *can* optionally use `m_timerFd` in conjunction with `select()`, `epoll()`, or a similar mechanism. This allows the external system to potentially sleep until `m_timerFd` becomes readable, indicating that a timer managed by `TimerMgr` *might* be ready to fire. The external system would then call `processTick()` to handle it. However, even if `m_timerFd` is used this way, regular calls to `processTick()` are still the fundamental driver if precise tick-based advancement is required (e.g., if no timers are set, `m_timerFd` might not fire, but `m_currentTick` should still advance if `processTick` is called).
*   **Thread Safety:** All public API methods (`createTimer`, `stopTimer`, `restartTimer`, `removeTimer`) and the `processTick()` method are made thread-safe through the use of a `std::mutex` (`m_mutex`). This protects shared data structures if these methods are called from different threads. For example, one thread might call API methods while another thread calls `processTick()`.

### Callback Execution
Callbacks are executed serially **within the thread that calls `processTick()`**.
If a callback takes a long time to complete, it will delay the processing of any subsequent timers within the same `processTick()` call and will block the caller's thread. Therefore, users should ensure that callbacks are lightweight and offload any lengthy or blocking operations to other threads.

## API Documentation

### `enum class BaseInterval`
Defines the possible base time granularities for the `TimerMgr`.
*   `MS10` (10 milliseconds)
*   `MS100` (100 milliseconds)
*   `MS250` (250 milliseconds)
*   `MS500` (500 milliseconds)
*   `S1` (1 second)

### Constructor
*   **`TimerMgr(BaseInterval interval)`**
    *   Creates a new Timer Manager instance.
    *   `interval`: The base interval for all timers managed by this instance.

### Destructor
*   **`~TimerMgr()`**
    *   Cleans up all managed timers and releases the `timer_fd`. Does not manage any internal threads.

### Timer Processing
*   **`void processTick()`**
    *   This method must be called periodically by the user to advance the timer manager's state and trigger callbacks for expired timers.
    *   Each call to `processTick()` increments an internal tick counter by one.
    *   It's recommended to call this at a frequency corresponding to the `BaseInterval` the `TimerMgr` was configured with (e.g., every 100ms if `BaseInterval::MS100` was used).

### Timer Creation
*   **`int64_t createTimer(std::function<void(void*)> callback, void* cookie, uint64_t intervalMs, bool isCyclic)`**
    *   Creates and starts a new timer.
    *   `callback`: The function to be called when the timer expires. Its signature must be `void(void*)`.
    *   `cookie`: A user-defined pointer that will be passed to the callback function.
    *   `intervalMs`: The duration of the timer in milliseconds. This value **must be greater than 0** and **must be a multiple** of the `TimerMgr`'s base interval (configured in its constructor).
    *   `isCyclic`: If `true`, the timer is periodic and will automatically reschedule itself after firing. If `false`, it is a one-shot timer and will be removed after firing (unless restarted).
    *   **Returns:** A unique `int64_t` timer ID if successful. Returns `-1` if an error occurs (e.g., `intervalMs` is invalid, callback is null).

### Timer Management
*   **`bool stopTimer(int64_t timerId)`**
    *   Stops an active timer. The timer remains in the manager and can be restarted. For cyclic timers, this prevents future firings. For one-shot timers, if called before expiry, it prevents the timer from firing.
    *   Returns `true` if the timer was found and stopped (or was already not running). Returns `false` if the timer ID is not found.
*   **`bool restartTimer(int64_t timerId)`**
    *   Restarts a timer.
        *   For a one-shot timer, it reschedules it to fire again after its original interval, relative to the current time.
        *   For a cyclic timer, it resets its current period, making it fire after its full interval from the current time.
    *   The timer is marked as running if it was previously stopped.
    *   Returns `true` if the timer was found and restarted, `false` otherwise.
*   **`bool removeTimer(int64_t timerId)`**
    *   Stops the timer (if running) and completely removes it from the manager. The timer ID becomes invalid after this call.
    *   Returns `true` if the timer was found and removed, `false` otherwise.

## Usage Examples

### Include Headers
```cpp
#include "common/timermgr.h" // Adjust this path based on your project structure
#include <iostream>
#include <chrono>
#include <thread>     // For std::this_thread::sleep_for in examples
```

### Basic One-Shot Timer
```cpp
// Callback function
void myCallback(void* cookie) {
    int* value = static_cast<int*>(cookie);
    std::cout << "Timer expired! Cookie value: " << *value << std::endl;
    // If you need to do more work, consider offloading to another thread
}

int main() {
    // Create a TimerMgr with a 100ms base interval
    swss::TimerMgr timerManager(swss::BaseInterval::MS100);

    int data = 42; // Data to be passed to the callback

    // Create a one-shot timer for 500ms (5 * 100ms base interval)
    // The interval must be a multiple of the base interval (100ms).
    int64_t timerId = timerManager.createTimer(myCallback, &data, 500, false);

    if (timerId == -1) {
        std::cerr << "Failed to create timer." << std::endl;
        return 1;
    }

    std::cout << "Timer created with ID: " << timerId << std::endl;

    std::cout << "Timer created with ID: " << timerId << std::endl;

    // External tick loop: Call processTick() periodically.
    // For this example, we'll call it 10 times, simulating 1 second (10 * 100ms).
    // In a real application, this would be part of an event loop or a dedicated ticker.
    for (int i = 0; i < 10; ++i) {
        timerManager.processTick(); // Advance timers by one base interval
        // Simulate the passage of time corresponding to the base interval
        std::this_thread::sleep_for(std::chrono::milliseconds(100)); 
    }

    // TimerMgr destructor will clean up when timerManager goes out of scope
    return 0;
}
```

### Cyclic Timer and Stopping
```cpp
struct AppContext {
    swss::TimerMgr* tm_ptr; // Pointer to the Timer Manager
    int64_t cyclicTimerId;
    int fireCount;
};

void cyclicCallback(void* cookie) {
    AppContext* ctx = static_cast<AppContext*>(cookie);
    ctx->fireCount++;
    std::cout << "Cyclic timer fired! Count: " << ctx->fireCount << std::endl;

    if (ctx->fireCount >= 5) {
        std::cout << "Stopping cyclic timer after " << ctx->fireCount << " executions." << std::endl;
        if (ctx->tm_ptr->stopTimer(ctx->cyclicTimerId)) {
            std::cout << "Timer successfully stopped." << std::endl;
            // To completely remove it:
            // ctx->tm_ptr->removeTimer(ctx->cyclicTimerId);
        } else {
            std::cerr << "Failed to stop timer." << std::endl;
        }
    }
}

int main() {
    swss::TimerMgr timerManager(swss::BaseInterval::MS250); // 250ms base interval
    AppContext app_ctx{&timerManager, 0, 0};

    // Create a cyclic timer for 250ms
    app_ctx.cyclicTimerId = timerManager.createTimer(cyclicCallback, &app_ctx, 250, true);

    if (app_ctx.cyclicTimerId == -1) {
        std::cerr << "Failed to create cyclic timer." << std::endl;
        return 1;
    }

    std::cout << "Cyclic timer created with ID: " << app_ctx.cyclicTimerId << std::endl;

    // External tick loop: Call processTick() periodically.
    // Loop for approximately 3 seconds (e.g., 3000ms / 250ms_base_interval = 12 ticks)
    // The callback itself will stop the timer after 5 executions.
    for (int i = 0; i < 15; ++i) { // Process enough ticks for it to fire 5 times and potentially more
        if (app_ctx.fireCount >= 5 && !timerManager.hasTimer(app_ctx.cyclicTimerId)) { // Example: check if timer was removed
             std::cout << "Timer removed by callback, exiting tick loop." << std::endl;
             break;
        }
        timerManager.processTick();
        std::this_thread::sleep_for(std::chrono::milliseconds(250)); // Simulate ticker interval
        if (app_ctx.fireCount >= 5 && i >= (5-1) ) { // Check if it should have been stopped by now
             // If timer is still there after 5 fires, maybe it wasn't stopped as expected
             // For this example, we just ensure we don't loop unnecessarily if it's stopped.
             auto it = timerManager.m_timers.find(app_ctx.cyclicTimerId); // Friend class or public getter needed for this check
             // This check is illustrative; real apps might have other conditions.
             // For this example, let's assume the callback handles removal/stopping.
        }
    }
    
    // If timer wasn't stopped by the callback (e.g., fireCount condition changed)
    // or if it was only stopped but not removed.
    // This part of the logic depends on how robust you want the main loop to be against callback logic.
    // For simplicity, we assume the callback correctly stops or removes.
    // If (timerManager.hasTimer(app_ctx.cyclicTimerId)) { // Requires a hasTimer method
    //    std::cout << "Main function explicitly removing timer post-loop." << std::endl;
    //    timerManager.removeTimer(app_ctx.cyclicTimerId);
    // }

    std::cout << "Exiting." << std::endl;
    return 0;
}

// Note: To make the cyclic example fully robust regarding checking if a timer still exists
// from outside (like in the main loop), TimerMgr might need a public `bool hasTimer(int64_t timerId)` method.
// The example above assumes callback handles this or relies on the loop processing enough ticks.
// For the provided example, I've added a hypothetical check comment.
// The `timerManager.m_timers.find` line is illustrative and won't compile as-is without friend access or a public getter.
// A simpler loop for the example just runs for a certain number of ticks:
/*
    for (int i = 0; i < 15; ++i) { // Process for a certain number of ticks
        if (app_ctx.fireCount >= 5) { // Assuming callback stops it, we can break early
            // Potentially call processTick a few more times to ensure stop takes effect if it happens in callback
            timerManager.processTick(); 
            break; 
        }
        timerManager.processTick();
        std::this_thread::sleep_for(std::chrono::milliseconds(250));
    }
    // Final cleanup if needed and timer still exists
    if (timerManager.hasTimer(app_ctx.cyclicTimerId)) { // Requires hasTimer method
        timerManager.removeTimer(app_ctx.cyclicTimerId);
    }
*/
```

## Testing Strategy

The Timer Manager library is unit-tested using the Google Test framework. The test suite is located in `tests/timermgr_ut.cpp`.

Key areas covered by the unit tests include:

*   **Correct Callback Invocation:** Verification that both one-shot and cyclic timers trigger their callbacks at the expected times and the expected number of times.
*   **Timer Lifecycle Management:** Thorough testing of `createTimer`, `stopTimer`, `restartTimer`, and `removeTimer` functionalities under various conditions.
*   **Context (`cookie`) Propagation:** Ensuring that the user-provided `cookie` is correctly passed to the callback function.
*   **Interval Validation:** Tests for invalid timer intervals, such as zero duration or durations not being a multiple of the `TimerMgr`'s base interval.
    **API Thread Safety:** Basic concurrent access tests to ensure public methods and `processTick()` are thread-safe when called from different threads.
*   **Functionality Across Different Base Intervals:** Tests are run with various `BaseInterval` settings (e.g., `MS10`, `S1`) to ensure consistent behavior.
*   **Edge Cases:** Testing includes scenarios like very short timer intervals (equal to the base interval) and sequences of rapid operations on timers.
    **External Tick Simulation:** Unit tests were updated to explicitly call `processTick()` to drive timer progression and trigger callbacks, simulating the external tick mechanism.

A `TestContext` helper struct is used within the unit tests, employing `std::condition_variable` and `std::atomic` counters, to reliably synchronize test assertions with the execution of timer callbacks.
