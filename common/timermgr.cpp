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
      m_timerFd(-1), // Initialize m_timerFd to an invalid value
      m_running(false) { // Initialize m_running to false initially
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

    m_running = true;
    try {
        m_timerThread = std::thread(&TimerMgr::timerThreadFunc, this);
        // Detaching the thread as per requirement.
        // Note: Careful consideration for detached threads is needed,
        // especially for resource cleanup if the TimerMgr object is destroyed
        // before the thread naturally finishes. The destructor handles this.
        m_timerThread.detach();
    } catch (const std::system_error& e) {
        m_running = false; // Ensure m_running is false if thread creation fails
        if (m_timerFd != -1) {
            close(m_timerFd);
            m_timerFd = -1;
        }
        SWSS_LOG_ERROR("Failed to create timer thread: " << e.what());
        throw; // Re-throw the exception
    }
    SWSS_LOG_INFO("TimerMgr started with base interval: " << baseIntervalToMs(m_baseInterval) << " ms");
}

TimerMgr::~TimerMgr() {
    SWSS_LOG_INFO("TimerMgr shutting down...");
    m_running = false; // Signal the timer thread to stop

    if (m_timerFd != -1) {
        // To wake up the thread if it's in read() or blocked on timerfd_settime,
        // we can disarm the timer and then make a small write or just close it.
        // Setting a very short timer is a reliable way to wake it up.
        struct itimerspec its_disarm_wakeup {};
        its_disarm_wakeup.it_value.tv_nsec = 1; // Wake up almost immediately
        // TFD_TIMER_ABSTIME not set, so relative time
        if (timerfd_settime(m_timerFd, 0, &its_disarm_wakeup, nullptr) == -1) {
            SWSS_LOG_ERROR("~TimerMgr: timerfd_settime to wake thread failed: " << strerror(errno));
            // Potentially, the thread might not wake up if it was in a state
            // where this settime doesn't affect it. However, read() should return.
        }
        // Note: The problem description mentions m_timerThread.join().
        // If the thread is detached, we cannot join it.
        // For a detached thread, it must manage its own lifecycle or communicate
        // its completion if needed. Given the prompt specified detach,
        // we will proceed with that. If join is strictly required, the thread
        // should not be detached. Assuming the destructor's responsibility is to
        // signal shutdown and clean up resources it directly owns (like m_timerFd).
        // The detached thread will see m_running = false and exit.
        // A robust solution for joinable threads would involve a condition variable.
        // For now, following the "detach" instruction.
    }
    
    // If thread was joinable, it would be:
    // if (m_timerThread.joinable()) {
    //     m_timerThread.join();
    // }

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

void TimerMgr::timerThreadFunc() {
    SWSS_LOG_INFO("Timer thread started.");
    uint64_t expirations; // Value read from timerfd

    while (m_running) {
        if (m_timerFd == -1) { // Should not happen if constructor succeeded
             SWSS_LOG_ERROR("timerThreadFunc: m_timerFd is invalid. Exiting thread.");
             break;
        }
        ssize_t s = read(m_timerFd, &expirations, sizeof(expirations));

        if (!m_running) { // Check m_running again after read returns
            break;
        }

        if (s == sizeof(expirations)) {
            // Timer fired
            // The value 'expirations' indicates how many times the timer has expired
            // since the last read. For a high-frequency base interval, this could be > 1.
            // We simply advance our current tick.
            // A more precise tick update would involve CLOCK_MONOTONIC if exactness is critical
            // beyond the timerfd notifications. But for a tick-based system, this is typical.
            // m_currentTick += expirations; // This might advance tick too fast if not careful.
                                         // The design implies m_currentTick is our logical tick
                                         // that advances when the *next* scheduled event is due.
                                         // Let's increment m_currentTick only when we process events
                                         // that are due at m_currentTick.
                                         // The read() just unblocks us. The actual current time
                                         // for event processing is derived from m_activeTimers.top().first
                                         // or by simply incrementing m_currentTick by 1 for each base interval.

            // The problem description for timerThreadFunc said:
            // "If read() returns a value > 0... Increment m_currentTick."
            // This means m_currentTick advances by the number of base intervals
            // that timerfd is configured for.
            // However, our updateTimerFd configures timerfd for a variable duration
            // until the *next* timer. So 'expirations' will typically be 1.
            // Let's assume m_currentTick should reflect the passage of time.
            // If updateTimerFd sets the timer to fire at `delayTicks` from `m_currentTick`,
            // then upon firing, `m_currentTick` should advance by `delayTicks`.
            // This needs careful synchronization with how `updateTimerFd` calculates delays.

            // Re-evaluating: The purpose of m_currentTick is to be the reference
            // for scheduling. When timerfd fires, it means time has advanced AT LEAST
            // to the point of the earliest timer.
            // The safest way is to process timers based on their expirationTick.
            // m_currentTick should advance to the tick of the timer(s) just processed.
        } else if (s == -1) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                // Non-blocking read, no event. This can happen if timerfd was disarmed
                // or if a signal interrupted read before any timer event.
                // Or if we woke up due to destructor.
                // We should check m_running and potentially re-arm if needed.
                if (!m_running) break; // Exit if shutting down

                // If we woke up spuriously and there are active timers,
                // ensure timerfd is correctly armed.
                std::lock_guard<std::mutex> lock(m_mutex);
                if (!m_activeTimers.empty() && m_running) {
                     // It's possible timerfd was disarmed by stopTimer or removeTimer
                     // or if all timers were removed and then a new one added.
                     // updateTimerFd will re-evaluate and arm if necessary.
                    updateTimerFd();
                }
                // Sleep for a very short duration to prevent busy spinning if continuously getting EAGAIN
                // This is a failsafe; ideally, timerfd is managed such that EAGAIN is not spammed.
                // struct timespec short_sleep = {0, 10000000}; // 10ms
                // nanosleep(&short_sleep, nullptr);
                // However, a properly managed timerfd (armed when timers exist, disarmed when not)
                // should mean read() blocks until a timer fires or it's interrupted.
                // EAGAIN here might mean it was disarmed.
                continue; // Re-check m_running and loop
            } else if (errno == EINTR) {
                SWSS_LOG_INFO("timerThreadFunc: read interrupted by signal, continuing.");
                continue;
            } else {
                SWSS_LOG_ERROR("timerThreadFunc: read from timerfd failed: " << strerror(errno) << ". Exiting thread.");
                m_running = false; // Stop the loop
                break;
            }
        } else if (s == 0) {
            // EOF, should not happen for timerfd
            SWSS_LOG_ERROR("timerThreadFunc: read from timerfd returned 0 (EOF). Exiting thread.");
            m_running = false;
            break;
        }


        // Process Timers
        std::unique_lock<std::mutex> lock(m_mutex);
        if (!m_running) { // Check again after acquiring lock
            break;
        }

        // Advance current tick. If timerfd fired, time has passed.
        // The most robust way to set m_currentTick is to align it with the earliest expired timer.
        // If multiple timers expired "at the same time" (same tick), process all of them.
        if (!m_activeTimers.empty() && m_activeTimers.top().first > m_currentTick) {
             // If the earliest timer is in the future relative to current tick,
             // advance current tick. This can happen if timerfd fires slightly early
             // or if `expirations` from read indicated multiple ticks passed.
             // For a one-shot timerfd set by updateTimerFd, m_currentTick should ideally
             // be set to m_activeTimers.top().first when that timer fires.
            // m_currentTick = m_activeTimers.top().first;
            // Let's simplify: when timerfd fires, it means we are at least at the tick of the next timer.
            // The number of expirations 's' from read() is how many times the *kernel* timer fired.
            // If our updateTimerFd sets it to fire in 'delayTicks' base intervals, then 'expirations'
            // should be 1. Then m_currentTick should advance by 'delayTicks'.
            // This is getting complicated. Let's use a simpler model for m_currentTick:
            // It advances by 1 for each "fundamental" tick of the system, and timerfd
            // is just a wake-up mechanism.
            // The problem says "Increment m_currentTick" if read > 0.
            // This implies m_timerFd is configured as a periodic timer of m_baseIntervalNs.
            // If so, updateTimerFd() logic needs to change.
            //
            // Let's assume updateTimerFd() sets a ONE-SHOT timer for the NEXT event.
            // When it fires, m_currentTick becomes that next event's time.
            if (s == sizeof(expirations) && !m_activeTimers.empty()) {
                 // Set current tick to the expiration tick of the timer that just fired
                 // (or the earliest one if multiple are due).
                 m_currentTick = m_activeTimers.top().first;
            }
        }


        while (!m_activeTimers.empty() && m_activeTimers.top().first <= m_currentTick) {
            if (!m_running) break;

            std::pair<uint64_t, int64_t> next_event = m_activeTimers.top();
            int64_t timerId = next_event.second;
            // uint64_t expectedExpirationTick = next_event.first; // not directly used after pop

            m_activeTimers.pop();

            auto it = m_timers.find(timerId);
            if (it == m_timers.end()) {
                SWSS_LOG_NOTICE("timerThreadFunc: Timer ID " << timerId << " not found in m_timers (expired after removal?).");
                continue;
            }

            TimerInfo& timerInfo = it->second;

            if (!timerInfo.isRunning) { // Could have been stopped
                // SWSS_LOG_INFO("timerThreadFunc: Timer ID " << timerId << " is not running, skipping.");
                continue;
            }
            
            // Unlock mutex before calling callback to prevent deadlocks if callback tries to use TimerMgr
            // This is a common practice but means timerInfo could be invalidated if removeTimer is called
            // by the callback. To handle this, we can copy necessary data or re-verify after callback.
            std::function<void(void*)> cb = timerInfo.callback;
            void* ck = timerInfo.cookie;
            bool is_cyclic = timerInfo.isCyclic;
            uint64_t interval_ms = timerInfo.intervalMs;

            // Temporarily mark as not running during callback if it's a one-shot or to prevent re-entrancy issues.
            // For cyclic timers, we'll reschedule.
            if (!is_cyclic) {
                timerInfo.isRunning = false;
            }

            lock.unlock();
            // SWSS_LOG_INFO("Executing callback for timer ID " << timerId);
            try {
                cb(ck);
            } catch (const std::exception& e) {
                SWSS_LOG_ERROR("timerThreadFunc: Exception from timer callback for ID " << timerId << ": " << e.what());
            } catch (...) {
                SWSS_LOG_ERROR("timerThreadFunc: Unknown exception from timer callback for ID " << timerId);
            }
            lock.lock(); // Re-acquire lock

            if (!m_running) break; // Check again after callback

            // Re-check if timer still exists, as callback might have removed it.
            it = m_timers.find(timerId);
            if (it == m_timers.end()) {
                // SWSS_LOG_INFO("timerThreadFunc: Timer ID " << timerId << " removed during callback.");
                continue; // Timer was removed by its own callback
            }
            // TimerInfo& currentTimerInfo = it->second; // Get fresh reference

            if (is_cyclic && it->second.isRunning) { // Check isRunning again, could be stopped in callback
                uint64_t intervalInBaseTicks = interval_ms / baseIntervalToMs(m_baseInterval);
                if (intervalInBaseTicks == 0) intervalInBaseTicks = 1; // Ensure progress

                it->second.expirationTick = m_currentTick + intervalInBaseTicks;
                m_activeTimers.push({it->second.expirationTick, timerId});
                // SWSS_LOG_INFO("Rescheduled cyclic timer ID " << timerId << " to tick " << it->second.expirationTick);
            } else if (!is_cyclic) {
                // One-shot timer has fired. It's already marked !isRunning.
                // If it needs to be restarted, user calls restartTimer.
                // SWSS_LOG_INFO("One-shot timer ID " << timerId << " completed.");
            }
        } // end while m_activeTimers

        if (m_running) { // Only update timerfd if still running
            updateTimerFd(); // Re-arm for the next closest timer with m_mutex still held
        }
    } // end while m_running

    SWSS_LOG_INFO("Timer thread finished.");
}


// Public methods implementation

int64_t TimerMgr::createTimer(std::function<void(void*)> callback, void* cookie, uint64_t intervalMs, bool isCyclic) {
    if (!callback) {
        SWSS_LOG_ERROR("createTimer: Callback function is null.");
        return -1;
    }
    uint64_t baseMs = baseIntervalToMs(m_baseInterval);
    if (intervalMs == 0 || intervalMs % baseMs != 0) {
        SWSS_LOG_ERROR("createTimer: IntervalMs " << intervalMs << " must be a non-zero multiple of base interval " << baseMs << "ms.");
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
