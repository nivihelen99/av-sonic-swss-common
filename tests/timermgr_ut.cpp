#include "gtest/gtest.h"
#include "common/timermgr.h" // Assuming this path is correct for the build system
#include <chrono>
#include <thread>
#include <atomic>
#include <vector>
#include <condition_variable>
#include <mutex>
#include <set>
#include <algorithm> // For std::generate, std::remove

using namespace swss;
using namespace std::chrono_literals;

// TestContext for synchronizing with callbacks
struct TestContext {
    std::atomic<int> execution_count{0};
    std::atomic<int> callback_id_sum{0}; // To check if specific timers fired
    void* last_cookie{nullptr};
    std::vector<int> execution_order_ids; // To track order if necessary
    
    std::mutex mtx;
    std::condition_variable cv;

    void reset() {
        execution_count = 0;
        callback_id_sum = 0;
        last_cookie = nullptr;
        execution_order_ids.clear();
    }

    void recordExecution(void* cookie, int id = 0) {
        execution_count++;
        callback_id_sum += id;
        last_cookie = cookie;
        {
            std::lock_guard<std::mutex> lock(mtx);
            execution_order_ids.push_back(id);
        }
        cv.notify_all();
    }

    bool waitForExecutions(int target_count, std::chrono::milliseconds timeout) {
        std::unique_lock<std::mutex> lock(mtx);
        return cv.wait_for(lock, timeout, [&]{ return execution_count.load() >= target_count; });
    }
    
    bool waitForExecutionCountExactly(int target_count, std::chrono::milliseconds timeout) {
        std::unique_lock<std::mutex> lock(mtx);
        return cv.wait_for(lock, timeout, [&]{ return execution_count.load() == target_count; });
    }
};

// General callback function for tests
void generalCallback(void* cookie) {
    if (cookie) {
        static_cast<TestContext*>(cookie)->recordExecution(cookie);
    }
}

// Callback that also records a virtual "timer ID" passed via cookie
// For this to work, the cookie needs to be more structured or we cast an int.
// Let's assume cookie itself can be an identifier for simplicity in some tests.
void idCallback(void* cookie) {
    if (cookie) {
        // Interpret cookie as an int for simplicity in sum check
        int id = reinterpret_cast<intptr_t>(cookie);
        // A real TestContext might be part of a larger structure or a map lookup
        // For now, using a global context for simplicity in this specific callback type.
        // This is not ideal. Better: TestContext* real_ctx = ((MyCookieStruct*)cookie)->ctx;
        // For this test, we'll assume a single, globally accessible TestContext for idCallback
        // or pass TestContext within a struct.
        // Let's simplify: TestContext is passed as the cookie, and we pass an additional id.
        // This would require changing the callback signature or TestContext.
        // Sticking to the provided TestContext and generalCallback mostly.
        // A simple way for idCallback:
        // static TestContext* id_test_ctx_ptr; // Needs to be set by test
        // id_test_ctx_ptr->recordExecution(cookie, id);
        // This is still messy.
        // Simpler: the cookie IS the TestContext, and it has a field for the id.
        // Or, the callback is a lambda capturing what it needs.
        // For now, generalCallback is primary.
    }
}


// Test Suite for baseIntervalToNs and baseIntervalToMs
TEST(TimerMgrUtilsTest, BaseIntervalConversions) {
    EXPECT_EQ(baseIntervalToMs(BaseInterval::MS10), 10ULL);
    EXPECT_EQ(baseIntervalToNs(BaseInterval::MS10), 10ULL * 1000000ULL);

    EXPECT_EQ(baseIntervalToMs(BaseInterval::MS100), 100ULL);
    EXPECT_EQ(baseIntervalToNs(BaseInterval::MS100), 100ULL * 1000000ULL);

    EXPECT_EQ(baseIntervalToMs(BaseInterval::MS250), 250ULL);
    EXPECT_EQ(baseIntervalToNs(BaseInterval::MS250), 250ULL * 1000000ULL);

    EXPECT_EQ(baseIntervalToMs(BaseInterval::MS500), 500ULL);
    EXPECT_EQ(baseIntervalToNs(BaseInterval::MS500), 500ULL * 1000000ULL);

    EXPECT_EQ(baseIntervalToMs(BaseInterval::S1), 1000ULL);
    EXPECT_EQ(baseIntervalToNs(BaseInterval::S1), 1000ULL * 1000000000ULL);

    // Test invalid argument (though typically this would be via an out-of-range enum, which C++ handles less gracefully for switch)
    // For this, we'd need to cast an int to BaseInterval.
    // EXPECT_THROW(baseIntervalToMs(static_cast<BaseInterval>(999)), std::invalid_argument);
    // This depends on how robust the enum checking is and if default case throws.
    // The current implementation does throw std::invalid_argument.
}

class TimerMgrTest : public ::testing::Test {
protected:
    TimerMgr* tm_ptr = nullptr;
    TestContext test_ctx;
    BaseInterval current_base_interval_ = BaseInterval::MS10; // Default

    // Default SetUp uses MS10. Call SetUp(interval) to override.
    void SetUp() override {
        SetUp(BaseInterval::MS10); // Default base interval
    }

    // Custom SetUp to allow different base intervals
    virtual void SetUp(BaseInterval interval) {
        if (tm_ptr) {
            delete tm_ptr;
            tm_ptr = nullptr;
        }
        current_base_interval_ = interval;
        tm_ptr = new TimerMgr(interval);
        test_ctx.reset();
    }

    void TearDown() override {
        delete tm_ptr;
        tm_ptr = nullptr;
    }

    uint64_t getBaseMs() const {
        return baseIntervalToMs(current_base_interval_);
    }
    
    // Helper to yield for a short period, allowing timer thread to catch up.
    // Prefer condition variables, but this can be a fallback or for specific timing tests.
    void yield(std::chrono::milliseconds duration = 1ms) {
        std::this_thread::sleep_for(duration);
    }
};

// Timer Creation Tests
TEST_F(TimerMgrTest, CreateOneShotTimer) {
    SetUp(BaseInterval::MS10);
    uint64_t interval_ms = 5 * getBaseMs(); // 50ms
    int64_t timer_id = tm_ptr->createTimer(generalCallback, &test_ctx, interval_ms, false);
    ASSERT_NE(timer_id, -1);

    EXPECT_TRUE(test_ctx.waitForExecutions(1, std::chrono::milliseconds(interval_ms * 2 + 50)));
    EXPECT_EQ(test_ctx.execution_count.load(), 1);
    EXPECT_EQ(test_ctx.last_cookie, &test_ctx);

    // Ensure it doesn't fire again
    std::this_thread::sleep_for(std::chrono::milliseconds(interval_ms * 2));
    EXPECT_EQ(test_ctx.execution_count.load(), 1);
    EXPECT_TRUE(tm_ptr->removeTimer(timer_id));
}

TEST_F(TimerMgrTest, CreateCyclicTimer) {
    SetUp(BaseInterval::MS10);
    uint64_t interval_ms = 3 * getBaseMs(); // 30ms
    int expected_fires = 3;
    int64_t timer_id = tm_ptr->createTimer(generalCallback, &test_ctx, interval_ms, true);
    ASSERT_NE(timer_id, -1);

    EXPECT_TRUE(test_ctx.waitForExecutions(expected_fires, std::chrono::milliseconds(interval_ms * (expected_fires + 1) + 50)));
    EXPECT_GE(test_ctx.execution_count.load(), expected_fires); // Could be more if test is slow
    EXPECT_LE(test_ctx.execution_count.load(), expected_fires + 1); // Check it's not firing too rapidly
    EXPECT_EQ(test_ctx.last_cookie, &test_ctx);

    EXPECT_TRUE(tm_ptr->stopTimer(timer_id));
    int count_after_stop = test_ctx.execution_count.load();
    std::this_thread::sleep_for(std::chrono::milliseconds(interval_ms * 2));
    EXPECT_EQ(test_ctx.execution_count.load(), count_after_stop); // Should not increase after stop

    EXPECT_TRUE(tm_ptr->removeTimer(timer_id));
}

TEST_F(TimerMgrTest, CreateTimerWithZeroInterval) {
    SetUp(BaseInterval::MS10);
    int64_t timer_id = tm_ptr->createTimer(generalCallback, &test_ctx, 0, false);
    EXPECT_EQ(timer_id, -1);
}

TEST_F(TimerMgrTest, CreateTimerWithNonMultipleInterval) {
    SetUp(BaseInterval::MS100); // Base interval 100ms
    int64_t timer_id = tm_ptr->createTimer(generalCallback, &test_ctx, 50, false); // 50ms is not a multiple of 100ms
    EXPECT_EQ(timer_id, -1);
}

TEST_F(TimerMgrTest, CreateTimerWithNullCallback) {
    SetUp(BaseInterval::MS10);
    int64_t timer_id = tm_ptr->createTimer(nullptr, &test_ctx, 100, false);
    EXPECT_EQ(timer_id, -1);
}


TEST_F(TimerMgrTest, CreateManyTimers) {
    SetUp(BaseInterval::MS10);
    const int num_timers = 100;
    std::vector<int64_t> timer_ids;
    uint64_t interval_ms = 2 * getBaseMs(); // 20ms for quick firing

    for (int i = 0; i < num_timers; ++i) {
        // Each timer gets its own context piece or unique ID if needed,
        // but here all use the same context and sum up.
        int64_t id = tm_ptr->createTimer(generalCallback, &test_ctx, interval_ms + (i % 5)*getBaseMs(), false); // Stagger slightly
        ASSERT_NE(id, -1);
        timer_ids.push_back(id);
    }

    EXPECT_TRUE(test_ctx.waitForExecutions(num_timers, std::chrono::milliseconds(interval_ms * num_timers / 10 + 200))); // Heuristic timeout
    EXPECT_EQ(test_ctx.execution_count.load(), num_timers);

    for (int64_t id : timer_ids) {
        EXPECT_TRUE(tm_ptr->removeTimer(id));
    }
}

// Timer Stopping Tests
TEST_F(TimerMgrTest, StopTimerBeforeExpiry) {
    SetUp(BaseInterval::MS10);
    uint64_t interval_ms = 10 * getBaseMs(); // 100ms
    int64_t timer_id = tm_ptr->createTimer(generalCallback, &test_ctx, interval_ms, false);
    ASSERT_NE(timer_id, -1);

    EXPECT_TRUE(tm_ptr->stopTimer(timer_id));
    yield(std::chrono::milliseconds(interval_ms * 2)); // Wait well past expected expiry
    
    EXPECT_EQ(test_ctx.execution_count.load(), 0);
    EXPECT_TRUE(tm_ptr->removeTimer(timer_id)); // Cleanup
}

TEST_F(TimerMgrTest, StopAlreadyStoppedTimer) {
    SetUp(BaseInterval::MS10);
    int64_t timer_id = tm_ptr->createTimer(generalCallback, &test_ctx, 100, false);
    ASSERT_NE(timer_id, -1);
    EXPECT_TRUE(tm_ptr->stopTimer(timer_id));
    EXPECT_TRUE(tm_ptr->stopTimer(timer_id)); // Stop again, should be no-op or indicate already stopped
                                             // Current impl: returns true if found and isRunning becomes false.
                                             // So, this should be true.
    EXPECT_TRUE(tm_ptr->removeTimer(timer_id));
}

TEST_F(TimerMgrTest, StopNonExistentTimer) {
    SetUp(BaseInterval::MS10);
    EXPECT_FALSE(tm_ptr->stopTimer(9999)); // Assuming 9999 is not a valid ID
}

// Timer Restarting Tests
TEST_F(TimerMgrTest, RestartOneShot_AfterExpiry) {
    SetUp(BaseInterval::MS10);
    uint64_t interval_ms = 3 * getBaseMs(); // 30ms
    int64_t timer_id = tm_ptr->createTimer(generalCallback, &test_ctx, interval_ms, false);
    ASSERT_NE(timer_id, -1);

    EXPECT_TRUE(test_ctx.waitForExecutions(1, std::chrono::milliseconds(interval_ms * 2 + 20)));
    EXPECT_EQ(test_ctx.execution_count.load(), 1);

    test_ctx.reset(); // Reset counter for next phase
    EXPECT_TRUE(tm_ptr->restartTimer(timer_id));
    EXPECT_TRUE(test_ctx.waitForExecutions(1, std::chrono::milliseconds(interval_ms * 2 + 20)));
    EXPECT_EQ(test_ctx.execution_count.load(), 1);
    
    EXPECT_TRUE(tm_ptr->removeTimer(timer_id));
}

TEST_F(TimerMgrTest, RestartOneShot_StoppedThenRestart) {
    SetUp(BaseInterval::MS10);
    uint64_t interval_ms = 5 * getBaseMs(); // 50ms
    int64_t timer_id = tm_ptr->createTimer(generalCallback, &test_ctx, interval_ms, false);
    ASSERT_NE(timer_id, -1);

    EXPECT_TRUE(tm_ptr->stopTimer(timer_id));
    yield(std::chrono::milliseconds(interval_ms)); // Ensure it would have fired if not stopped
    EXPECT_EQ(test_ctx.execution_count.load(), 0);

    EXPECT_TRUE(tm_ptr->restartTimer(timer_id));
    EXPECT_TRUE(test_ctx.waitForExecutions(1, std::chrono::milliseconds(interval_ms * 2 + 20)));
    EXPECT_EQ(test_ctx.execution_count.load(), 1);

    EXPECT_TRUE(tm_ptr->removeTimer(timer_id));
}

TEST_F(TimerMgrTest, RestartOneShot_Running) {
    SetUp(BaseInterval::MS10);
    uint64_t interval_ms = 10 * getBaseMs(); // 100ms
    int64_t timer_id = tm_ptr->createTimer(generalCallback, &test_ctx, interval_ms, false);
    ASSERT_NE(timer_id, -1);

    yield(std::chrono::milliseconds(interval_ms / 2)); // Let it run for a bit
    EXPECT_TRUE(tm_ptr->restartTimer(timer_id)); // Restart it
    // It should fire 'interval_ms' after the restart, not 'interval_ms / 2'
    
    // Total wait time: interval_ms/2 (initial) + interval_ms (restarted)
    // We expect 1 execution total. waitForExecutions checks '>=1'.
    // Need to ensure it doesn't fire early.
    // Wait for slightly less than new expiry time from now.
    std::this_thread::sleep_for(std::chrono::milliseconds(interval_ms - getBaseMs()));
    EXPECT_EQ(test_ctx.execution_count.load(), 0); // Should not have fired yet

    EXPECT_TRUE(test_ctx.waitForExecutions(1, std::chrono::milliseconds(getBaseMs() * 3 + 20))); // Wait for the remaining time + buffer
    EXPECT_EQ(test_ctx.execution_count.load(), 1);

    EXPECT_TRUE(tm_ptr->removeTimer(timer_id));
}


TEST_F(TimerMgrTest, RestartCyclicTimer) {
    SetUp(BaseInterval::MS10);
    uint64_t interval_ms = 3 * getBaseMs(); // 30ms
    int64_t timer_id = tm_ptr->createTimer(generalCallback, &test_ctx, interval_ms, true);
    ASSERT_NE(timer_id, -1);

    EXPECT_TRUE(test_ctx.waitForExecutions(2, std::chrono::milliseconds(interval_ms * 3 + 20))); // Let it fire twice
    
    test_ctx.reset(); // Reset for counting after restart
    EXPECT_TRUE(tm_ptr->restartTimer(timer_id));
    
    EXPECT_TRUE(test_ctx.waitForExecutions(2, std::chrono::milliseconds(interval_ms * 3 + 20))); // Should fire twice more after restart
    EXPECT_GE(test_ctx.execution_count.load(), 2);

    EXPECT_TRUE(tm_ptr->stopTimer(timer_id));
    EXPECT_TRUE(tm_ptr->removeTimer(timer_id));
}

TEST_F(TimerMgrTest, RestartNonExistentTimer) {
    SetUp(BaseInterval::MS10);
    EXPECT_FALSE(tm_ptr->restartTimer(9999));
}

// Timer Removal Tests
TEST_F(TimerMgrTest, RemoveTimerBeforeExpiry) {
    SetUp(BaseInterval::MS10);
    uint64_t interval_ms = 10 * getBaseMs(); // 100ms
    int64_t timer_id = tm_ptr->createTimer(generalCallback, &test_ctx, interval_ms, false);
    ASSERT_NE(timer_id, -1);

    EXPECT_TRUE(tm_ptr->removeTimer(timer_id));
    yield(std::chrono::milliseconds(interval_ms * 2)); // Wait well past expected expiry
    EXPECT_EQ(test_ctx.execution_count.load(), 0);

    EXPECT_FALSE(tm_ptr->removeTimer(timer_id)); // Already removed
}

TEST_F(TimerMgrTest, RemoveTimerAfterExpiry_OneShot) {
    SetUp(BaseInterval::MS10);
    uint64_t interval_ms = 3 * getBaseMs(); // 30ms
    int64_t timer_id = tm_ptr->createTimer(generalCallback, &test_ctx, interval_ms, false);
    ASSERT_NE(timer_id, -1);

    EXPECT_TRUE(test_ctx.waitForExecutions(1, std::chrono::milliseconds(interval_ms * 2 + 20)));
    EXPECT_EQ(test_ctx.execution_count.load(), 1);
    
    EXPECT_TRUE(tm_ptr->removeTimer(timer_id));
    EXPECT_FALSE(tm_ptr->removeTimer(timer_id)); // Already removed
}

TEST_F(TimerMgrTest, RemoveNonExistentTimer) {
    SetUp(BaseInterval::MS10);
    EXPECT_FALSE(tm_ptr->removeTimer(9999));
}

TEST_F(TimerMgrTest, OperationsAfterRemoval) {
    SetUp(BaseInterval::MS10);
    int64_t timer_id = tm_ptr->createTimer(generalCallback, &test_ctx, 100, false);
    ASSERT_NE(timer_id, -1);

    EXPECT_TRUE(tm_ptr->removeTimer(timer_id));
    EXPECT_FALSE(tm_ptr->stopTimer(timer_id));
    EXPECT_FALSE(tm_ptr->restartTimer(timer_id));
    EXPECT_FALSE(tm_ptr->removeTimer(timer_id));
}

// Cookie Handling
TEST_F(TimerMgrTest, CookieVerification) {
    SetUp(BaseInterval::MS10);
    struct MyCookie { int valA; double valB; };
    MyCookie c1 = {10, 20.5};
    MyCookie c2 = {30, 40.5};

    int64_t id1 = tm_ptr->createTimer(generalCallback, &c1, 2 * getBaseMs(), false);
    ASSERT_NE(id1, -1);
    int64_t id2 = tm_ptr->createTimer(generalCallback, &c2, 3 * getBaseMs(), false);
    ASSERT_NE(id2, -1);

    // Wait for both
    EXPECT_TRUE(test_ctx.waitForExecutions(2, 100ms));
    EXPECT_EQ(test_ctx.execution_count.load(), 2);
    // last_cookie will be from the timer that fired last. This is non-deterministic here.
    // To test cookies properly, each callback should verify its own cookie.
    // We can use separate TestContexts or a more complex TestContext.

    // Refined cookie test:
    TestContext ctx1, ctx2;
    id1 = tm_ptr->createTimer(generalCallback, &ctx1, 2 * getBaseMs(), false); // 20ms
    ASSERT_NE(id1, -1);
    id2 = tm_ptr->createTimer(generalCallback, &ctx2, 3 * getBaseMs(), false); // 30ms
    ASSERT_NE(id2, -1);

    EXPECT_TRUE(ctx1.waitForExecutions(1, 50ms));
    EXPECT_EQ(ctx1.last_cookie, &ctx1);
    EXPECT_EQ(ctx1.execution_count.load(), 1);

    EXPECT_TRUE(ctx2.waitForExecutions(1, 60ms));
    EXPECT_EQ(ctx2.last_cookie, &ctx2);
    EXPECT_EQ(ctx2.execution_count.load(), 1);
    
    tm_ptr->removeTimer(id1);
    tm_ptr->removeTimer(id2);
}


// Thread Safety Tests (Basic)
TEST_F(TimerMgrTest, ConcurrentTimerCreations) {
    SetUp(BaseInterval::MS10);
    const int num_threads = 10;
    const int timers_per_thread = 10;
    std::vector<std::thread> threads;
    std::atomic<int> created_timer_ids_count{0};
    std::vector<int64_t> all_timer_ids; // Needs mutex if threads modify it
    std::mutex vector_mutex;

    uint64_t interval_ms = 2 * getBaseMs(); // 20ms

    for (int i = 0; i < num_threads; ++i) {
        threads.emplace_back([&]() {
            std::vector<int64_t> local_ids;
            for (int j = 0; j < timers_per_thread; ++j) {
                // Each timer callback needs to know its TestContext.
                // For simplicity, all use the global test_ctx.
                int64_t id = tm_ptr->createTimer(generalCallback, &test_ctx, interval_ms + (j%2)*getBaseMs(), false);
                if (id != -1) {
                    created_timer_ids_count++;
                    local_ids.push_back(id);
                }
            }
            std::lock_guard<std::mutex> lock(vector_mutex);
            all_timer_ids.insert(all_timer_ids.end(), local_ids.begin(), local_ids.end());
        });
    }

    for (auto& t : threads) {
        t.join();
    }

    int total_timers = num_threads * timers_per_thread;
    EXPECT_EQ(created_timer_ids_count.load(), total_timers);
    EXPECT_TRUE(test_ctx.waitForExecutions(total_timers, std::chrono::milliseconds(interval_ms * timers_per_thread + 500))); // Heuristic
    EXPECT_EQ(test_ctx.execution_count.load(), total_timers);

    for (int64_t id : all_timer_ids) {
        tm_ptr->removeTimer(id); // Clean up
    }
}

TEST_F(TimerMgrTest, ConcurrentStopAndRestart) {
    SetUp(BaseInterval::MS10);
    const int num_ops_threads = 5;
    uint64_t interval_ms = 20 * getBaseMs(); // 200ms, long enough to be manipulated

    // Create a few timers to operate on
    std::vector<int64_t> timer_ids;
    std::vector<TestContext> contexts(5);
    for(int i=0; i<5; ++i) {
        contexts[i].reset();
        int64_t id = tm_ptr->createTimer(generalCallback, &contexts[i], interval_ms, true); // Cyclic
        ASSERT_NE(id, -1);
        timer_ids.push_back(id);
    }

    std::vector<std::thread> threads;
    for (int i = 0; i < num_ops_threads; ++i) {
        threads.emplace_back([&, i]() {
            int64_t target_id = timer_ids[i % timer_ids.size()];
            for(int k=0; k<5; ++k) { // Multiple operations per thread
                tm_ptr->stopTimer(target_id);
                yield(getBaseMs()); // small yield
                tm_ptr->restartTimer(target_id);
                yield(getBaseMs());
            }
        });
    }

    for (auto& t : threads) {
        t.join();
    }

    // Check that timers are still running and firing after concurrent ops
    // This is hard to assert definitively without knowing the exact state.
    // We expect them to be running. Let's check for some activity.
    std::this_thread::sleep_for(interval_ms * 2); // Let them run for a couple of cycles

    for(int i=0; i<5; ++i) {
        EXPECT_GE(contexts[i].execution_count.load(), 1) << "Timer " << timer_ids[i] << " should have fired at least once after ops.";
        tm_ptr->stopTimer(timer_ids[i]); // Stop them
        tm_ptr->removeTimer(timer_ids[i]); // Clean up
    }
}


// Edge Cases & Robustness
TEST_F(TimerMgrTest, TimerWithShortestInterval) {
    SetUp(BaseInterval::MS10);
    uint64_t interval_ms = getBaseMs(); // 10ms
    int64_t timer_id = tm_ptr->createTimer(generalCallback, &test_ctx, interval_ms, false);
    ASSERT_NE(timer_id, -1);

    EXPECT_TRUE(test_ctx.waitForExecutions(1, std::chrono::milliseconds(interval_ms * 3 + 20)));
    EXPECT_EQ(test_ctx.execution_count.load(), 1);
    tm_ptr->removeTimer(timer_id);
}

TEST_F(TimerMgrTest, SequenceOfOperations) {
    SetUp(BaseInterval::MS10);
    uint64_t interval_ms = 5 * getBaseMs(); // 50ms
    int64_t timer_id = tm_ptr->createTimer(generalCallback, &test_ctx, interval_ms, false);
    ASSERT_NE(timer_id, -1);

    // Stop -> Start (Restart)
    EXPECT_TRUE(tm_ptr->stopTimer(timer_id));
    yield(interval_ms);
    EXPECT_EQ(test_ctx.execution_count.load(), 0);
    EXPECT_TRUE(tm_ptr->restartTimer(timer_id));
    EXPECT_TRUE(test_ctx.waitForExecutions(1, std::chrono::milliseconds(interval_ms * 2 + 20)));
    EXPECT_EQ(test_ctx.execution_count.load(), 1);

    // Stop -> Start (Restart) again
    test_ctx.reset();
    EXPECT_TRUE(tm_ptr->stopTimer(timer_id)); // Timer is one-shot, already fired and implicitly stopped by not being cyclic.
                                             // So stopTimer should reflect it's not 'running' for next callback.
                                             // Or, if it's about being in m_timers, it is.
                                             // Current stopTimer logic: sets isRunning = false.
    yield(interval_ms);
    EXPECT_EQ(test_ctx.execution_count.load(), 0);
    EXPECT_TRUE(tm_ptr->restartTimer(timer_id)); // Restart means it will run again
    EXPECT_TRUE(test_ctx.waitForExecutions(1, std::chrono::milliseconds(interval_ms * 2 + 20)));
    EXPECT_EQ(test_ctx.execution_count.load(), 1);

    // Remove
    EXPECT_TRUE(tm_ptr->removeTimer(timer_id));
    EXPECT_FALSE(tm_ptr->removeTimer(timer_id)); // Fails as already removed
}

// Test with a different base interval
class TimerMgrTestDifferentBase : public TimerMgrTest {
};

TEST_F(TimerMgrTestDifferentBase, OneShotWithS1Base) {
    SetUp(BaseInterval::S1); // 1 second base interval
    uint64_t interval_ms = 2 * getBaseMs(); // 2000ms
    
    auto start_time = std::chrono::steady_clock::now();
    int64_t timer_id = tm_ptr->createTimer(generalCallback, &test_ctx, interval_ms, false);
    ASSERT_NE(timer_id, -1);

    EXPECT_TRUE(test_ctx.waitForExecutions(1, std::chrono::milliseconds(interval_ms + 500))); // Wait up to 2.5s
    auto end_time = std::chrono::steady_clock::now();
    
    EXPECT_EQ(test_ctx.execution_count.load(), 1);
    auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end_time - start_time);
    
    // Check if it fired approximately after interval_ms
    EXPECT_GE(duration.count(), interval_ms);
    // Allow some slack for scheduling, timer thread processing, etc.
    // For a 2s timer with 1s base, it might fire between 2s and 3s.
    EXPECT_LT(duration.count(), interval_ms + getBaseMs() + 500ms); // interval + base_interval + buffer
    
    tm_ptr->removeTimer(timer_id);
}

// Main function for GTest
int main(int argc, char **argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
