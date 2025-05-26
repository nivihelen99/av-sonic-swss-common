#pragma once

#include <functional>
#include <vector>
#include <mutex>
#include <map>
#include <queue> // Or other suitable container for priority queue
#include <cstdint> // For uint64_t, int64_t
#include <thread>  // For std::thread

// Forward declaration if needed by Selectable
// namespace swss { class Selectable; }

namespace swss {

enum class BaseInterval {
    MS10,
    MS100,
    MS250,
    MS500,
    S1,
};

// Convert BaseInterval to nanoseconds
uint64_t baseIntervalToNs(BaseInterval interval);
// Convert BaseInterval to milliseconds
uint64_t baseIntervalToMs(BaseInterval interval);


class TimerMgr {
public:
    TimerMgr(BaseInterval interval);
    ~TimerMgr();

    TimerMgr(const TimerMgr&) = delete;
    TimerMgr& operator=(const TimerMgr&) = delete;

    int64_t createTimer(std::function<void(void*)> callback, void* cookie, uint64_t intervalMs, bool isCyclic);
    bool stopTimer(int64_t timerId);
    bool restartTimer(int64_t timerId); // For one-shot, restarts. For cyclic, resets current period.
    bool removeTimer(int64_t timerId);

private:
    struct TimerInfo {
        int64_t id;
        std::function<void(void*)> callback;
        void* cookie;
        uint64_t intervalMs;      // Original interval in ms
        uint64_t expirationTick;  // Tick at which this timer expires
        bool isCyclic;
        bool isRunning;
        // Add any other necessary fields
    };

    void timerThreadFunc();
    void updateTimerFd(); // Helper to re-arm the timerfd based on the next expiring timer

    BaseInterval m_baseInterval;
    uint64_t m_baseIntervalNs; // Base interval in nanoseconds
    uint64_t m_currentTick;    // Current tick count

    int m_timerFd;
    bool m_running;
    std::thread m_timerThread;
    std::mutex m_mutex;

    std::map<int64_t, TimerInfo> m_timers; // Stores all timers by ID
    // Priority queue stores pairs of (expirationTick, timerId)
    // Using std::pair, smallest expirationTick will be at the top
    std::priority_queue<std::pair<uint64_t, int64_t>, 
                        std::vector<std::pair<uint64_t, int64_t>>, 
                        std::greater<std::pair<uint64_t, int64_t>>> m_activeTimers;

    int64_t m_nextTimerId;
};

} // namespace swss
