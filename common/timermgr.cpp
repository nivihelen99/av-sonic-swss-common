#include "timermgr.h"
#include <sys/timerfd.h>
#include <unistd.h>    // For read, close
#include <stdexcept>   // For std::runtime_error
#include <iostream>    // For std::cerr (logging placeholder)
#include <cstring>     // For strerror
#include <cerrno>      // For errno

// Define SWSS_LOG macros if not available, for placeholder logging
#ifndef SWSS_LOG_ERROR
#define SWSS_LOG_ERROR(x) std::cerr << "ERROR: " << x << std::endl
#endif
#ifndef SWSS_LOG_NOTICE
#define SWSS_LOG_NOTICE(x) std::cout << "NOTICE: " << x << std::endl
#endif
#ifndef SWSS_LOG_INFO
#define SWSS_LOG_INFO(x) std::cout << "INFO: " << x << std::endl
#endif


namespace swss {

// Helper Functions
uint64_t baseIntervalToNs(BaseInterval interval) {
    switch (interval) {
        case BaseInterval::MS10:  return 10ULL * 1000000ULL;
        case BaseInterval::MS100: return 100ULL * 1000000ULL;
        case BaseInterval::MS250: return 250ULL * 1000000ULL;
        case BaseInterval::MS500: return 500ULL * 1000000ULL;
        case BaseInterval::S1:    return 1ULL * 1000000000ULL;
        default:
            throw std::invalid_argument("Invalid BaseInterval value");
    }
}

uint64_t baseIntervalToMs(BaseInterval interval) {
    switch (interval) {
        case BaseInterval::MS10:  return 10ULL;
        case BaseInterval::MS100: return 100ULL;
        case BaseInterval::MS250: return 250ULL;
        case BaseInterval::MS500: return 500ULL;
        case BaseInterval::S1:    return 1000ULL;
        default:
            throw std::invalid_argument("Invalid BaseInterval value");
    }
}

TimerMgr::TimerMgr(BaseInterval interval)
    : m_baseInterval(interval),
      m_currentTick(0),
      m_nextTimerId(0),
      m_timerFd(-1) { // m_running removed
    try {
        m_baseIntervalNs = baseIntervalToNs(m_baseInterval);
    } catch (const std::invalid_argument& e) {
        SWSS_LOG_ERROR("Failed to initialize baseIntervalNs: " << e.what());
        throw; // Re-throw the exception
    }

    m_timerFd = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK);
    if (m_timerFd == -1) {
        SWSS_LOG_ERROR("timerfd_create failed: " << strerror(errno));
        throw std::runtime_error("timerfd_create failed");
    }

    // m_running and m_timerThread related code removed.
    // SWSS_LOG_INFO("TimerMgr initialized with base interval: " << baseIntervalToMs(m_baseInterval) << " ms");
    // Initial call to updateTimerFd to arm/disarm based on current state (likely disarmed)
    std::lock_guard<std::mutex> lock(m_mutex); // updateTimerFd expects lock to be held
    updateTimerFd();
}

TimerMgr::~TimerMgr() {
    // SWSS_LOG_INFO("TimerMgr shutting down...");
    // m_running and thread signaling code removed.

    if (m_timerFd != -1) {
        close(m_timerFd);
        m_timerFd = -1;
    }

    std::lock_guard<std::mutex> lock(m_mutex);
    m_timers.clear();
    // Clear priority queue
    std::priority_queue<std::pair<uint64_t, int64_t>,
                        std::vector<std::pair<uint64_t, int64_t>>,
                        std::greater<std::pair<uint64_t, int64_t>>> empty_pq;
    m_activeTimers.swap(empty_pq);
    SWSS_LOG_INFO("TimerMgr shutdown complete.");
}


void TimerMgr::updateTimerFd() {
    // This function should be called with m_mutex HELD
    struct itimerspec its_new {};

    if (m_activeTimers.empty()) {
        // Disarm timer if no active timers
        // its_new is already zeroed (equivalent to its_disarm)
        if (m_timerFd != -1 && timerfd_settime(m_timerFd, 0, &its_new, nullptr) == -1) {
            SWSS_LOG_ERROR("updateTimerFd: disarm timerfd_settime failed: " << strerror(errno));
        }
        // SWSS_LOG_INFO("TimerFd disarmed.");
        return;
    }

    uint64_t nextExpirationTick = m_activeTimers.top().first;
    uint64_t delayTicks = 0;

    if (nextExpirationTick > m_currentTick) {
        delayTicks = nextExpirationTick - m_currentTick;
    } else {
        // Timer should have already fired or fire immediately
        delayTicks = 1; // Smallest possible delay, effectively "now"
    }

    uint64_t delayNs = delayTicks * m_baseIntervalNs;

    its_new.it_value.tv_sec = delayNs / 1000000000ULL;
    its_new.it_value.tv_nsec = delayNs % 1000000000ULL;
    // it_interval is left at zero for a one-shot timerfd behavior;
    // we manually reschedule in timerThreadFunc.

    if (m_timerFd != -1) {
        // SWSS_LOG_INFO("Arming TimerFd: delayTicks=" << delayTicks << ", delayNs=" << delayNs);
        if (timerfd_settime(m_timerFd, 0, &its_new, nullptr) == -1) {
            SWSS_LOG_ERROR("updateTimerFd: arm timerfd_settime failed: " << strerror(errno));
        }
    }
}

// void TimerMgr::timerThreadFunc() { ... } // Entire function removed


void TimerMgr::processTick() {
    // Step 1: Consume event from timer_fd.
    uint64_t fd_expirations = 0;
    ssize_t ret = read(m_timerFd, &fd_expirations, sizeof(fd_expirations));

    if (ret == -1) {
        if (errno == EINTR) {
            // SWSS_LOG_INFO("TimerMgr: read from timer_fd interrupted by signal.");
            return; // Wait for next call
        } else if (errno != EAGAIN && errno != EWOULDBLOCK) {
            SWSS_LOG_ERROR("TimerMgr: Failed to read from timer_fd: " << strerror(errno));
            // Depending on severity, could throw or just log.
        }
        // If EAGAIN/EWOULDBLOCK or other error, fd_expirations remains 0 or is uninitialized.
        // Best to ensure it's 0 if we proceed.
        fd_expirations = 0;
    } else if (ret == 0) {
        SWSS_LOG_NOTICE("TimerMgr: Read 0 bytes from timer_fd, unexpected EOF.");
        fd_expirations = 0;
    }
    // fd_expirations is now set. Its value isn't critical for tick advancement here,
    // but reading clears the m_timerFd for external pollers.

    std::vector<std::pair<std::function<void(void*)>, void*>> callbacksToRun;

    { // Mutex scope
        std::lock_guard<std::mutex> lock(m_mutex);

        // Step 2: Advance current tick.
        m_currentTick++;

        // Step 3: Process timers
        while (!m_activeTimers.empty() && m_activeTimers.top().first <= m_currentTick) {
            int64_t timerId = m_activeTimers.top().second;
            uint64_t actualExpirationTick = m_activeTimers.top().first;
            m_activeTimers.pop();

            auto it = m_timers.find(timerId);
            if (it == m_timers.end()) { // Timer removed
                continue;
            }
            if (!it->second.isRunning) { // Timer stopped
                continue;
            }

            TimerInfo& timerInfo = it->second;
            
            callbacksToRun.push_back({timerInfo.callback, timerInfo.cookie});

            if (timerInfo.isCyclic) {
                uint64_t baseMs = baseIntervalToMs(m_baseInterval); // Cache or pass if performance critical
                uint64_t intervalInTicks = timerInfo.intervalMs / baseMs;
                if (intervalInTicks == 0) intervalInTicks = 1; // Ensure progress

                timerInfo.expirationTick = actualExpirationTick + intervalInTicks;
                // If it's still in the past due to long processing or very short interval,
                // schedule from m_currentTick to ensure it's in the future.
                if (timerInfo.expirationTick <= m_currentTick) {
                     timerInfo.expirationTick = m_currentTick + intervalInTicks;
                }
                m_activeTimers.push({timerInfo.expirationTick, timerId});
            } else {
                timerInfo.isRunning = false;
            }
        }

        // Step 4: Re-arm the timer_fd for the next actual event.
        updateTimerFd();
    } // Mutex releases here

    // Step 5: Run callbacks outside of the mutex lock
    for (const auto& cb_pair : callbacksToRun) {
        if (cb_pair.first) {
            try {
                cb_pair.first(cb_pair.second);
            } catch (const std::exception &e) {
                SWSS_LOG_ERROR("TimerMgr: Exception from timer callback: " << e.what());
            } catch (...) {
                SWSS_LOG_ERROR("TimerMgr: Unknown exception from timer callback");
            }
        }
    }
}

int64_t TimerMgr::createTimer(std::function<void(void*)> callback, void* cookie, uint64_t intervalMs, bool isCyclic) {
    if (!callback) {
        SWSS_LOG_ERROR("createTimer: Callback function is null.");
        return -1;
    }
    uint64_t baseMs = baseIntervalToMs(m_baseInterval);
    if (intervalMs == 0) {
        SWSS_LOG_ERROR("createTimer: IntervalMs cannot be 0.");
        return -1;
    }
    if (intervalMs % baseMs != 0) {
        SWSS_LOG_ERROR("createTimer: IntervalMs " << intervalMs << " must be a multiple of base interval " << baseMs << "ms.");
        return -1;
    }

    std::lock_guard<std::mutex> lock(m_mutex);

    int64_t timerId = m_nextTimerId++;
    TimerInfo newTimer;
    newTimer.id = timerId;
    newTimer.callback = callback;
    newTimer.cookie = cookie;
    newTimer.intervalMs = intervalMs;
    newTimer.isCyclic = isCyclic;
    newTimer.isRunning = true;

    uint64_t intervalInBaseTicks = intervalMs / baseMs;
    if (intervalInBaseTicks == 0) { // Should not happen if intervalMs > 0 and intervalMs % baseMs == 0, unless baseMs > intervalMs
        intervalInBaseTicks = 1;    // Ensure timer fires at least one tick away
    }
    newTimer.expirationTick = m_currentTick + intervalInBaseTicks;

    m_timers[timerId] = newTimer;
    m_activeTimers.push({newTimer.expirationTick, timerId});

    // SWSS_LOG_INFO("Created timer ID " << timerId << ", interval " << intervalMs << "ms, expires at tick " << newTimer.expirationTick);
    updateTimerFd(); // Re-evaluate and potentially re-arm timerfd

    return timerId;
}

bool TimerMgr::stopTimer(int64_t timerId) {
    std::lock_guard<std::mutex> lock(m_mutex);
    auto it = m_timers.find(timerId);
    if (it == m_timers.end()) {
        SWSS_LOG_ERROR("stopTimer: Timer ID " << timerId << " not found.");
        return false;
    }

    if (!it->second.isRunning) {
        // SWSS_LOG_INFO("stopTimer: Timer ID " << timerId << " is already stopped.");
        return true; // Or false if "was not running" is an error
    }

    it->second.isRunning = false;
    // SWSS_LOG_INFO("Stopped timer ID " << timerId);

    // Note: Timer remains in m_timers and m_activeTimers.
    // timerThreadFunc will ignore it when it pops from m_activeTimers if isRunning is false.
    // To truly remove it from m_activeTimers requires rebuilding the queue or a more complex structure.
    // For simplicity, we mark as not running. If it's critical to remove from PQ,
    // a common strategy is to add a generation number or use a custom PQ that allows removal.
    // Given the current structure, it will be "lazily" removed from PQ when it expires.
    // This is acceptable for many use cases.
    // To make stop more immediate in terms of timerfd, we could call updateTimerFd.
    updateTimerFd(); // May disarm or reschedule if this was the next timer.
    return true;
}

bool TimerMgr::restartTimer(int64_t timerId) {
    std::lock_guard<std::mutex> lock(m_mutex);
    auto it = m_timers.find(timerId);
    if (it == m_timers.end()) {
        SWSS_LOG_ERROR("restartTimer: Timer ID " << timerId << " not found.");
        return false;
    }

    TimerInfo& timerInfo = it->second;
    timerInfo.isRunning = true; // Ensure it's marked as running

    uint64_t baseMs = baseIntervalToMs(m_baseInterval);
    uint64_t intervalInBaseTicks = timerInfo.intervalMs / baseMs;
    if (intervalInBaseTicks == 0) intervalInBaseTicks = 1;

    timerInfo.expirationTick = m_currentTick + intervalInBaseTicks;

    m_activeTimers.push({timerInfo.expirationTick, timerId});
    // SWSS_LOG_INFO("Restarted timer ID " << timerId << ", new expiration tick " << timerInfo.expirationTick);

    updateTimerFd();
    return true;
}

bool TimerMgr::removeTimer(int64_t timerId) {
    std::lock_guard<std::mutex> lock(m_mutex);
    auto it = m_timers.find(timerId);
    if (it == m_timers.end()) {
        SWSS_LOG_ERROR("removeTimer: Timer ID " << timerId << " not found.");
        return false;
    }

    it->second.isRunning = false; // Mark as not running to prevent execution if it's about to fire
    m_timers.erase(it);
    // SWSS_LOG_INFO("Removed timer ID " << timerId << " from m_timers.");

    // Removing from m_activeTimers (priority_queue) is tricky.
    // Standard std::priority_queue doesn't support efficient removal of arbitrary elements.
    // Strategies:
    // 1. Rebuild the priority queue (inefficient if frequent removals).
    // 2. Mark the timer as "removed" or "cancelled" (e.g., in m_timers or a separate set).
    //    When timerThreadFunc pops it, it checks this mark and ignores it.
    //    This is what `isRunning = false` and removing from `m_timers` achieves.
    //    The entry will still be in m_activeTimers but will be skipped.
    // This seems to be the implicit design here.

    updateTimerFd(); // Re-evaluate and re-arm timerfd
    return true;
}

} // namespace swss
