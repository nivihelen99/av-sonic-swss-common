#include "topiceventsubscriber.h"
#include "common/logger.h" // For SWSS_LOG_*
#include <chrono> // For timeouts
#include <thread> // For sleep

// Make sure EVENT_PARAM_ENTITY_ID is available. It's defined in common/events.h
// and common/topiceventsubscriber.h includes common/events.h.

namespace swss {

TopicEventSubscriber::TopicEventSubscriber(const std::string& event_source, bool use_cache, int recv_timeout, const event_subscribe_sources_t *sources)
    : m_event_handle(nullptr), m_default_recv_timeout(recv_timeout) {

    event_subscribe_sources_t actual_sources_list;
    const event_subscribe_sources_t *sources_to_pass = sources;

    if (sources == nullptr) {
        if (!event_source.empty()) {
            actual_sources_list.push_back(event_source);
            sources_to_pass = &actual_sources_list;
        }
        // If sources is nullptr and event_source is empty, events_init_subscriber will subscribe to all.
    }
    // If sources is not nullptr, it's used directly, and event_source is conceptual.

    m_event_handle = events_init_subscriber(use_cache, m_default_recv_timeout, sources_to_pass);

    if (!m_event_handle) {
        SWSS_LOG_ERROR("Failed to initialize event subscriber. Event source hint: %s", event_source.c_str());
        // Consider throwing an exception here for critical failure
    }
}

TopicEventSubscriber::~TopicEventSubscriber() {
    if (m_event_handle) {
        events_deinit_subscriber(m_event_handle);
    }
}

void TopicEventSubscriber::subscribeTopic(const std::string& entity_topic) {
    m_subscribed_topics.insert(entity_topic);
}

void TopicEventSubscriber::unsubscribeTopic(const std::string& entity_topic) {
    m_subscribed_topics.erase(entity_topic);
}

bool TopicEventSubscriber::isTopicSubscribed(const std::string& entity_topic) const {
    return m_subscribed_topics.count(entity_topic) > 0;
}

int TopicEventSubscriber::receive(event_receive_op_t& evt, int timeout_ms_override) {
    if (!m_event_handle) {
        SWSS_LOG_WARN("Event handle not initialized, cannot receive events.");
        return ERR_OTHER; // Defined in events_common.h or use a local constant
    }

    int current_timeout_for_call = (timeout_ms_override != -1) ? timeout_ms_override : m_default_recv_timeout;

    auto call_start_time = std::chrono::steady_clock::now();

    while (true) {
        // Determine timeout for the underlying event_receive call.
        // If current_timeout_for_call is blocking (-1), then event_receive can also block.
        // If current_timeout_for_call is non-blocking (0), event_receive should be non-blocking.
        // If current_timeout_for_call has a specific duration, event_receive can use a portion or be non-blocking.
        // For simplicity, we'll let event_receive use its configured default timeout (m_default_recv_timeout)
        // or a short non-blocking poll if we are managing a larger timeout window.

        int underlying_receive_timeout = m_default_recv_timeout;
        if (current_timeout_for_call == 0) { // If overall call is non-blocking
            underlying_receive_timeout = 0;
        } else if (current_timeout_for_call > 0) { // If overall call has a timeout
             // Use a small polling interval for event_receive to check elapsed time frequently
            underlying_receive_timeout = std::min(10, current_timeout_for_call); // Poll every 10ms or less
        }
        // If current_timeout_for_call is -1 (blocking), underlying_receive_timeout remains m_default_recv_timeout.

        // Temporarily set timeout for event_receive if needed, then restore.
        // This assumes event_receive can have its timeout changed or m_event_handle has such a method.
        // The current events.h API (events_init_subscriber) sets timeout at init.
        // For this implementation, we rely on the timeout set during events_init_subscriber.
        // The loop for timeout handling is thus for when m_default_recv_timeout is 0 or small.

        int ret = event_receive(m_event_handle, evt);

        if (ret == 0) { // Successfully received an event
            if (m_subscribed_topics.empty()) {
                return 0; // No specific topics, pass through
            }
            auto it = evt.params.find(EVENT_PARAM_ENTITY_ID);
            if (it != evt.params.end()) {
                if (m_subscribed_topics.count(it->second)) {
                    return 0; // Matched topic
                } else {
                    SWSS_LOG_DEBUG("TopicEventSubscriber: Filtered out event for entity_id: %s", it->second.c_str());
                }
            } else {
                SWSS_LOG_DEBUG("TopicEventSubscriber: Event without '%s', filtering out as specific topics are subscribed.", EVENT_PARAM_ENTITY_ID.c_str());
            }
        } else if (ret > 0) { // Timeout from event_receive (typically means no message if timeout was >0)
            // This 'ret > 0' is the zmq_errno if event_receive returns it, or a custom timeout indicator.
            // Assuming event_receive returns >0 for timeout (like EAGAIN for ZMQ)
            if (current_timeout_for_call == 0) return ret; // Immediate timeout if configured
            // if current_timeout_for_call is -1, we continue, relying on event_receive's blocking
            // if current_timeout_for_call > 0, we check elapsed time below
        } else { // Error from event_receive (ret < 0)
            SWSS_LOG_ERROR("TopicEventSubscriber: Error %d from event_receive", ret);
            return ret;
        }

        // Check for overall timeout if a specific duration is set for this call
        if (current_timeout_for_call > 0) {
            auto current_time = std::chrono::steady_clock::now();
            auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(current_time - call_start_time).count();
            if (elapsed_ms >= current_timeout_for_call) {
                return 1; // General timeout indication (consistent with event_receive for timeout)
            }
            // To prevent busy spin if underlying event_receive is non-blocking (timeout 0)
            // and we haven't reached current_timeout_for_call yet.
            if (ret > 0 && m_default_recv_timeout == 0) { // ret > 0 was timeout from non-blocking event_receive
                 std::this_thread::sleep_for(std::chrono::milliseconds(10)); // Small sleep
            }
        } else if (current_timeout_for_call == 0 && ret > 0) {
            // Non-blocking call, and event_receive indicated no data (timeout-like)
            return ret;
        }
        // If current_timeout_for_call == -1 (infinite), and ret > 0 (timeout from event_receive with its own timeout),
        // this means event_receive itself timed out, so we just loop again.
        // If event_receive was also infinite timeout, it would block until message or error.
    }
}

} // namespace swss
