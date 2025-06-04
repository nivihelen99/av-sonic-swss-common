#ifndef COMMON_ADVANCED_PUBSUB_TYPES_H_
#define COMMON_ADVANCED_PUBSUB_TYPES_H_

#include <string>
#include <chrono>
#include <vector>
#include <queue>
#include <functional> // For std::function and std::greater
#include "common/table.h" // For swss::KeyOpFieldsValuesTuple

namespace common {

// Enum for message delivery modes
enum class DeliveryMode {
    AT_MOST_ONCE,
    AT_LEAST_ONCE,
    EXACTLY_ONCE
};

// Enum for message status (for ACK tracking)
enum class MessageStatus {
    PENDING,
    ACKED,
    NACKED
};

// Structure to represent a message
struct Message {
    // Redis key prefixes and field names for ACK status tracking
    static constexpr const char* MSG_STATUS_PREFIX = "MSG_STATUS:";          // Hash: message_id -> status details
    static constexpr const char* CORRELATION_IDS_PREFIX = "CORR_IDS:";       // Set: correlation_id -> set of message_ids
    static constexpr const char* PENDING_MESSAGES_BY_TIME_KEY = "PENDING_MSG_TS"; // Sorted Set: timestamp -> message_id (for timeouts)

    static constexpr const char* MSG_STAT_FIELD_STATUS = "status";
    static constexpr const char* MSG_STAT_FIELD_TIMESTAMP = "timestamp"; // Last update timestamp
    static constexpr const char* MSG_STAT_FIELD_CORR_ID = "correlation_id";
    static constexpr const char* MSG_STAT_FIELD_DELIVERY_MODE = "delivery_mode";
    static constexpr const char* MSG_STAT_FIELD_TABLE_NAME = "table_name"; // Original producer table

    std::string id;
    int priority; // 0-9, 0 being lowest priority, 9 highest
    std::string original_key; // Added to store the original key for the message
    std::string content;
    std::chrono::time_point<std::chrono::system_clock> timestamp;
    int retry_count;
    std::string correlation_id;
    DeliveryMode delivery_mode;

    static constexpr const char* MSG_PAYLOAD_FIELD = "advanced_message_payload";

    // Comparator for priority queue
    struct CompareMessages {
        bool operator()(const Message& a, const Message& b) const {
            if (a.priority != b.priority) {
                return a.priority < b.priority; // Higher numeric priority means it's "larger" for max-heap
            }
            return a.timestamp > b.timestamp; // Older timestamp (smaller value) means "larger" for max-heap (FIFO)
        }
    };
};


// Class template for a priority queue of messages
template <typename TMessage = Message>
class PriorityQueue {
public:
    PriorityQueue() : total_size_(0) {}

    void push(const TMessage& message) {
        main_queue_.push(message);
        total_size_++;
    }

    TMessage pop() {
        if (empty()) {
            throw std::out_of_range("PriorityQueue is empty");
        }
        TMessage msg = main_queue_.top();
        main_queue_.pop();
        total_size_--;
        return msg;
    }

    bool empty() const {
        return main_queue_.empty();
    }

    size_t size() const {
        return total_size_;
    }

    size_t getPriorityLevelSize(int priority) const {
        // Placeholder: Actual implementation would require iterating or separate tracking.
        return 0;
    }

private:
    std::priority_queue<TMessage, std::vector<TMessage>, typename TMessage::CompareMessages> main_queue_;
    size_t total_size_;

    // static constexpr size_t MAX_TOTAL_MEMORY = 1000000;
    // static constexpr size_t MAX_MEMORY_PER_PRIORITY = 100000;
};

// Enum for filter types
enum class FilterType {
    KEY_GLOB,
    JSON_PATH,
    CUSTOM_PREDICATE
};

// Base structure for filter configurations
struct FilterConfig {
    FilterType type;
    std::string filter_id; // Unique identifier for a filter instance

    virtual ~FilterConfig() = default; // Good practice for base classes with virtual functions or inheritance
};

// Derived structure for Key Glob filtering
struct KeyGlobFilterConfig : public FilterConfig {
    std::string glob_pattern;

    KeyGlobFilterConfig() { type = FilterType::KEY_GLOB; }
};

// Derived structure for JSON Path filtering
struct JsonPathFilterConfig : public FilterConfig {
    std::string json_path_expression;
    std::string expected_value; // Can be a JSON string to compare against

    JsonPathFilterConfig() { type = FilterType::JSON_PATH; }
};

// Derived structure for Custom Predicate filtering
struct CustomPredicateFilterConfig : public FilterConfig {
    std::function<bool(const swss::KeyOpFieldsValuesTuple&)> predicate;

    CustomPredicateFilterConfig() { type = FilterType::CUSTOM_PREDICATE; }
};

} // namespace common


namespace swss { // Keep PubSubConfig in swss namespace as it's closely tied to swss components
    struct PubSubConfig {
        int default_priority = 5;
        int max_queue_depth = 10000;
        int ack_timeout_ms = 30000;    // 30 seconds
        int max_retries = 3;
        common::DeliveryMode delivery_mode = common::DeliveryMode::AT_LEAST_ONCE;
        int dead_letter_retention_hours = 7 * 24; // 7 days
        bool enable_message_persistence = true;
        int filter_cache_size = 1000;

        // Add more fields as they become necessary from JSON schema or requirements
        // Example:
        // std::string specific_producer_option;
        // int specific_consumer_option;
    };
} // namespace swss


#endif // COMMON_ADVANCED_PUBSUB_TYPES_H_
