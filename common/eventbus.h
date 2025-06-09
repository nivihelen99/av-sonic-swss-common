#ifndef COMMON_EVENTBUS_H_
#define COMMON_EVENTBUS_H_

#include <string>
#include <vector>
#include <unordered_map>
#include <functional>
#include <any> // For event data
#include <memory> // For shared_ptr

namespace common {

// Forward declaration
class EventBus;

// A generic Event structure
struct Event {
    std::string topic;
    std::any data; // Allows any kind of data to be sent

    Event(const std::string& t, std::any d) : topic(t), data(std::move(d)) {}
};

// Type alias for a subscriber callback function
// Takes a const reference to an Event
using SubscriberCallback = std::function<void(const Event&)>;

// Optional: A filter function type for subscribers
// Takes a const reference to an Event and returns true if the event should be delivered
using EventFilter = std::function<bool(const Event&)>;

// Subscription class to manage individual subscriptions (e.g., for unsubscribing)
class Subscription {
public:
    Subscription(std::shared_ptr<EventBus> bus, size_t id) : bus_(bus), id_(id) {}
    // ~Subscription(); // In a real scenario, this would unsubscribe

    // Method to unsubscribe (optional, could also be handled by EventBus)
    // void unsubscribe(); // Placeholder

private:
    std::weak_ptr<EventBus> bus_; // Weak pointer to avoid circular dependencies
    size_t id_; // Unique ID for this subscription

    friend class EventBus; // Allow EventBus to access private members
};


class EventBus : public std::enable_shared_from_this<EventBus> {
public:
    EventBus() = default;
    ~EventBus() = default;

    // Prevent copying and assignment
    EventBus(const EventBus&) = delete;
    EventBus& operator=(const EventBus&) = delete;

    // Method to publish an event to a topic
    void publish(const std::string& topic, std::any data) {
        Event event(topic, std::move(data));
        publish(event);
    }

    void publish(const Event& event) {
        const auto& topic_subscribers = subscribers_[event.topic];
        for (const auto& sub_pair : topic_subscribers) {
            const auto& subscriber_info = sub_pair.second;
            if (subscriber_info.filter) {
                if (subscriber_info.filter(event)) {
                    subscriber_info.callback(event);
                }
            } else {
                subscriber_info.callback(event);
            }
        }
    }

    // Method to subscribe to a topic with an optional filter
    // Returns a Subscription object that can be used to unsubscribe (or an ID)
    std::shared_ptr<Subscription> subscribe(const std::string& topic, SubscriberCallback callback, EventFilter filter = nullptr) {
        size_t subscription_id = next_subscription_id_++;
        subscribers_[topic][subscription_id] = {callback, filter};
        return std::make_shared<Subscription>(shared_from_this(), subscription_id);
    }

    // Method to unsubscribe (example, might need more robust implementation)
    void unsubscribe(const std::string& topic, size_t subscription_id) {
        auto it = subscribers_.find(topic);
        if (it != subscribers_.end()) {
            it->second.erase(subscription_id);
            if (it->second.empty()) {
                subscribers_.erase(it);
            }
        }
    }

    void unsubscribe(std::shared_ptr<Subscription> sub) {
        if (!sub) return;
        // This is a simplified way to find the topic.
        // A real implementation might store topic in Subscription or have a reverse map.
        for (auto& topic_pair : subscribers_) {
            if (topic_pair.second.count(sub->id_)) {
                unsubscribe(topic_pair.first, sub->id_);
                return;
            }
        }
    }


private:
    struct SubscriberInfo {
        SubscriberCallback callback;
        EventFilter filter; // Optional filter
    };

    // Map of topics to a map of subscription IDs to subscribers
    std::unordered_map<std::string, std::unordered_map<size_t, SubscriberInfo>> subscribers_;
    size_t next_subscription_id_ = 0;
};

} // namespace common

#endif // COMMON_EVENTBUS_H_
