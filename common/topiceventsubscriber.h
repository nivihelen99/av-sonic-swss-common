#pragma once

#include <string>
#include <vector>
#include <set>
#include "common/events.h" // For event_handle_t, event_receive_op_t, EVENT_PARAM_ENTITY_ID

namespace swss {

class TopicEventSubscriber {
public:
    TopicEventSubscriber(const std::string& event_source, bool use_cache = false, int recv_timeout = -1, const event_subscribe_sources_t *sources = nullptr);
    ~TopicEventSubscriber();

    // Disable copy and assignment
    TopicEventSubscriber(const TopicEventSubscriber&) = delete;
    TopicEventSubscriber& operator=(const TopicEventSubscriber&) = delete;

    void subscribeTopic(const std::string& entity_topic);
    void unsubscribeTopic(const std::string& entity_topic);
    bool isTopicSubscribed(const std::string& entity_topic) const;

    int receive(event_receive_op_t& evt, int timeout_ms = -1); // timeout_ms here overrides constructor default if provided

private:
    event_handle_t m_event_handle;
    std::set<std::string> m_subscribed_topics;
    int m_default_recv_timeout; // Store the default timeout from constructor
};

} // namespace swss
