# Timer Manager Library (`TimerMgr`)

## Overview

The `TimerMgr` library provides an efficient timer mechanism for applications requiring multiple timers with minimal resource overhead. Instead of using one system `timer_fd` per timer, `TimerMgr` utilizes a single `timer_fd` for all managed timers, making it suitable for scenarios with a large number of concurrent timers.

**Key Features:**

*   **Configurable Base Interval:** Timer durations are multiples of a chosen base interval (e.g., 10ms, 100ms), defining the timer granularity.
*   **One-Shot and Cyclic Timers:** Supports both timers that fire once and timers that fire repeatedly.
*   **User-Provided Context:** Allows arbitrary `void*` data (cookie) to be passed to timer callbacks.
*   **Thread-Safety:** Public API methods are thread-safe.

**Problem Solved:**
This library addresses the inefficiency and resource consumption issues that arise when managing many timers by creating a separate `timer_fd` (or equivalent system resource) for each individual timer. By multiplexing all timers onto a single `timer_fd`, it significantly reduces system load.

## Design

### Core Idea
The library's core design revolves around a single POSIX timer created using `timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK)`. This timer is dynamically armed to fire at the expiration time of the soonest active timer.

### Base Interval
All timer durations must be non-zero multiples of a configurable base interval. This interval is set when the `TimerMgr` instance is created and determines the fundamental "tick" rate and granularity of the timers. Available base intervals are: 10ms, 100ms, 250ms, 500ms, and 1 second.

### Timer Tick
The library internally operates on discrete "ticks." Each tick corresponds to one base interval. Timer expirations are scheduled and processed based on these ticks. `m_currentTick` tracks the current tick count since the `TimerMgr` started.

### Data Structures
*   `std::map<int64_t, TimerInfo> m_timers`: This map stores detailed information for every timer, including its callback, cookie, interval, type (one-shot/cyclic), and current state. Timers are uniquely identified by an `int64_t` ID.
*   `std::priority_queue<std::pair<uint64_t, int64_t>> m_activeTimers`: This priority queue stores pairs of `(expirationTick, timerId)`. It is ordered by `expirationTick`, ensuring that the timer that will expire next is always at the top. This allows for O(1) access to the next due timer.

### Thread Model
*   **Background Thread (`m_timerThread`):** A dedicated background thread is responsible for all timer management and callback execution.
*   **Waiting for Expirations:** This thread waits for the main `timer_fd` to become readable by calling `read()`.
*   **Processing Timers:** When the `timer_fd` signals an expiration (i.e., `read()` returns), the thread determines the current tick. It then iterates through `m_activeTimers`, processing all timers whose `expirationTick` is less than or equal to the current tick.
*   **Callback Execution:** Timer callbacks are executed directly within this `m_timerThread`.
*   **API Thread Safety:** All public API methods (e.g., `createTimer`, `stopTimer`) are made thread-safe through the use of a `std::mutex` (`m_mutex`), which protects shared data structures.

### Callback Execution
It is important to note that **callbacks are executed serially within the single timer thread.** If a callback takes a long time to complete, it will delay the processing of subsequent timer expirations and the execution of their callbacks. Therefore, users should ensure that callbacks are lightweight and offload any lengthy or blocking operations to other threads.

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
    *   Stops the timer thread, cleans up all managed timers, and releases the `timer_fd`.

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

    // Keep the main thread alive long enough for the timer to fire
    std::this_thread::sleep_for(std::chrono::seconds(1)); // Wait for 1 second

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

    // Let the timer run for a few seconds
    std::this_thread::sleep_for(std::chrono::seconds(3));

    // If not stopped by callback, explicitly remove it (or stop it)
    if (app_ctx.fireCount < 5) {
       std::cout << "Main function removing timer." << std::endl;
       timerManager.removeTimer(app_ctx.cyclicTimerId);
    }
    
    std::cout << "Exiting." << std::endl;
    return 0;
}
```

## Testing Strategy

The Timer Manager library is unit-tested using the Google Test framework. The test suite is located in `tests/timermgr_ut.cpp`.

Key areas covered by the unit tests include:

*   **Correct Callback Invocation:** Verification that both one-shot and cyclic timers trigger their callbacks at the expected times and the expected number of times.
*   **Timer Lifecycle Management:** Thorough testing of `createTimer`, `stopTimer`, `restartTimer`, and `removeTimer` functionalities under various conditions.
*   **Context (`cookie`) Propagation:** Ensuring that the user-provided `cookie` is correctly passed to the callback function.
*   **Interval Validation:** Tests for invalid timer intervals, such as zero duration or durations not being a multiple of the `TimerMgr`'s base interval.
*   **API Thread Safety:** Basic concurrent access tests to ensure public methods are thread-safe.
*   **Functionality Across Different Base Intervals:** Tests are run with various `BaseInterval` settings (e.g., `MS10`, `S1`) to ensure consistent behavior.
*   **Edge Cases:** Testing includes scenarios like very short timer intervals (equal to the base interval) and sequences of rapid operations on timers.

A `TestContext` helper struct is used within the unit tests, employing `std::condition_variable` and `std::atomic` counters, to reliably synchronize test assertions with the asynchronous execution of timer callbacks.
