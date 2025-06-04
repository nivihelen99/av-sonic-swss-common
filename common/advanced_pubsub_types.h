#ifndef COMMON_ADVANCED_PUBSUB_TYPES_H_
#define COMMON_ADVANCED_PUBSUB_TYPES_H_

#include <string>
#include <chrono>
#include <vector>
#include <queue>
#include <functional> // For std::function and std::greater
#include "common/table.h" // For swss::KeyOpFieldsValuesTuple

namespace common {

/**
 * @brief Defines the Quality of Service (QoS) for message delivery.
 */
enum class DeliveryMode {
    AT_MOST_ONCE,  ///< Fire-and-forget. Message may be lost if consumer is unavailable or crashes.
    AT_LEAST_ONCE, ///< Guarantees message will be delivered at least once. Duplicates may occur.
    EXACTLY_ONCE   ///< Guarantees message will be delivered exactly once. (Most complex to achieve)
};

/**
 * @brief Represents the acknowledgment status of a message, primarily for reliable delivery modes.
 */
enum class MessageStatus {
    PENDING, ///< Message has been sent but not yet acknowledged.
    ACKED,   ///< Message has been successfully processed and acknowledged by the consumer.
    NACKED   ///< Message processing failed, and it was negatively acknowledged.
};

/**
 * @brief Represents a message in the advanced Pub/Sub system.
 *
 * This struct encapsulates all data and metadata associated with a message,
 * including its content, priority, delivery guarantees, and tracking information.
 */
struct Message {
    // Redis key prefixes and field names for ACK status tracking
    static constexpr const char* MSG_STATUS_PREFIX = "MSG_STATUS:";          ///< Prefix for Redis keys storing message status hashes.
    static constexpr const char* CORRELATION_IDS_PREFIX = "CORR_IDS:";       ///< Prefix for Redis keys storing sets of message IDs per correlation ID.
    static constexpr const char* PENDING_MESSAGES_BY_TIME_KEY = "PENDING_MSG_TS"; ///< Key for Redis Sorted Set storing pending message IDs by timestamp for timeout processing.

    // Field names within the MSG_STATUS hash
    static constexpr const char* MSG_STAT_FIELD_STATUS = "status";                ///< Field name for the message's MessageStatus.
    static constexpr const char* MSG_STAT_FIELD_TIMESTAMP = "timestamp";          ///< Field name for the last update timestamp of the status.
    static constexpr const char* MSG_STAT_FIELD_CORR_ID = "correlation_id";       ///< Field name for the message's correlation ID.
    static constexpr const char* MSG_STAT_FIELD_DELIVERY_MODE = "delivery_mode";  ///< Field name for the message's DeliveryMode.
    static constexpr const char* MSG_STAT_FIELD_TABLE_NAME = "table_name";        ///< Field name for the original producer's table name.

    std::string id;                             ///< Unique identifier for the message (e.g., UUID).
    int priority = 5;                         ///< Message priority (0-9, 9 is highest). Default is 5.
    std::string original_key;                   ///< The original key provided by the producer for this message.
    std::string original_table_name;            ///< Table name of the original producer.
    std::string content;                        ///< The message payload, typically a JSON string representing key-value pairs.
    std::chrono::time_point<std::chrono::system_clock> timestamp; ///< Timestamp of message creation or last significant update.
    int retry_count = 0;                      ///< Number of times delivery has been attempted (especially for NACK/requeue scenarios).
    std::string correlation_id;                 ///< Optional ID for tracking a group of related messages or a batch.
    DeliveryMode delivery_mode = DeliveryMode::AT_LEAST_ONCE; ///< Requested delivery mode for the message.

    // DLQ specific fields (populated when a message is moved to DLQ)
    std::string dlq_reason;                     ///< Reason why the message was moved to the Dead Letter Queue.
    long long dlq_timestamp_ms = 0;             ///< Timestamp (in milliseconds since epoch) when the message was moved to DLQ.
    std::string last_nack_error_message;        ///< If NACKed, this stores the error message associated with the last NACK.

    /// Field name used when the entire serialized Message object is stored as a single value in Redis (e.g., in DLQ).
    static constexpr const char* MSG_PAYLOAD_FIELD = "advanced_message_payload";

    /**
     * @brief Comparator for use in std::priority_queue.
     * Orders messages by priority (higher numeric value = higher priority).
     * For messages with the same priority, older messages (smaller timestamp) get higher precedence (FIFO).
     */
    struct CompareMessages {
        bool operator()(const Message& a, const Message& b) const {
            if (a.priority != b.priority) {
                return a.priority < b.priority; // Higher numeric priority means it's "larger" for max-heap
            }
            return a.timestamp > b.timestamp; // Older timestamp (smaller value) means "larger" for max-heap (FIFO)
        }
    };
};


/**
 * @brief A template class for a priority queue that stores messages.
 *
 * Internally, it uses `std::priority_queue` with a custom comparator defined in `TMessage`
 * (e.g., `Message::CompareMessages`) to ensure messages are ordered by priority,
 * and then by timestamp for FIFO behavior among messages of the same priority.
 * @tparam TMessage The type of message to store, defaults to `common::Message`.
 */
template <typename TMessage = Message>
class PriorityQueue {
public:
    /**
     * @brief Default constructor.
     */
    PriorityQueue() : total_size_(0) {}

    /**
     * @brief Pushes a message onto the priority queue.
     * @param message The message to push.
     */
    void push(const TMessage& message) {
        main_queue_.push(message);
        total_size_++;
    }

    /**
     * @brief Pops the highest priority message from the queue.
     *
     * If multiple messages have the same highest priority, FIFO is applied based on timestamp.
     * @return TMessage The popped message.
     * @throws std::out_of_range if the queue is empty.
     */
    TMessage pop() {
        if (empty()) {
            throw std::out_of_range("PriorityQueue is empty");
        }
        TMessage msg = main_queue_.top();
        main_queue_.pop();
        total_size_--;
        return msg;
    }

    /**
     * @brief Checks if the priority queue is empty.
     * @return true if the queue is empty, false otherwise.
     */
    bool empty() const {
        return main_queue_.empty();
    }

    /**
     * @brief Gets the total number of messages in the priority queue.
     * @return size_t The total number of messages.
     */
    size_t size() const {
        return total_size_;
    }

    /**
     * @brief Placeholder for getting the size of a specific priority level.
     * @param priority The priority level.
     * @return size_t Currently returns 0 (not implemented).
     * @note Actual implementation would require iterating or separate tracking.
     */
    size_t getPriorityLevelSize(int priority) const {
        // Placeholder: Actual implementation would require iterating or separate tracking.
        return 0;
    }

private:
    std::priority_queue<TMessage, std::vector<TMessage>, typename TMessage::CompareMessages> main_queue_; ///< The internal priority queue.
    size_t total_size_; ///< Total number of messages in the queue.

    // TODO: Consider mechanisms to bound memory per priority level or total.
    // static constexpr size_t MAX_TOTAL_MEMORY = 1000000;
    // static constexpr size_t MAX_MEMORY_PER_PRIORITY = 100000;
};

/**
 * @brief Defines the types of filters that can be applied to messages.
 */
enum class FilterType {
    KEY_GLOB,         ///< Filter based on a glob pattern matching the message key.
    JSON_PATH,        ///< Filter based on a JSONPath expression evaluated against message content.
    CUSTOM_PREDICATE  ///< Filter based on a custom user-defined predicate function.
};

/**
 * @brief Base structure for filter configurations.
 */
struct FilterConfig {
    FilterType type;        ///< The type of the filter.
    std::string filter_id;  ///< Unique identifier for a filter instance.

    /**
     * @brief Virtual destructor for proper cleanup of derived types.
     */
    virtual ~FilterConfig() = default;
};

/**
 * @brief Configuration for a key-based glob pattern filter.
 * @details Messages pass this filter if their `original_key` matches the `glob_pattern`.
 */
struct KeyGlobFilterConfig : public FilterConfig {
    std::string glob_pattern; ///< The glob pattern to match against the message key.

    KeyGlobFilterConfig() { type = FilterType::KEY_GLOB; }
};

/**
 * @brief Configuration for a content-based JSONPath filter.
 * @details Messages pass this filter if the value extracted by `json_path_expression`
 *          from their (JSON) content matches `expected_value`.
 */
struct JsonPathFilterConfig : public FilterConfig {
    std::string json_path_expression; ///< The JSONPath expression to evaluate.
    std::string expected_value;       ///< The value expected from JSONPath evaluation (can be a JSON string).

    JsonPathFilterConfig() { type = FilterType::JSON_PATH; }
};

/**
 * @brief Configuration for a custom predicate filter.
 * @details Messages pass this filter if the provided `predicate` function returns true.
 *          The predicate operates on the raw `KeyOpFieldsValuesTuple` as received by the consumer.
 *          For filtering on `common::Message`, use `AdvancedConsumerTable::setCustomFilter`.
 */
struct CustomPredicateFilterConfig : public FilterConfig {
    std::function<bool(const swss::KeyOpFieldsValuesTuple&)> predicate; ///< The custom predicate function.

    CustomPredicateFilterConfig() { type = FilterType::CUSTOM_PREDICATE; }
};

} // namespace common


namespace swss {
/**
 * @brief Configuration settings for advanced Pub/Sub producers and consumers.
 * @details This struct holds various parameters that control the behavior of
 *          AdvancedProducerTable and AdvancedConsumerTable. Defaults are provided,
 *          and these can be overridden by loading settings from CONFIG_DB via
 *          `swss::loadPubSubConfig`.
 */
struct PubSubConfig {
    int default_priority = 5;                   ///< Default message priority if not specified.
    int max_queue_depth = 10000;                ///< Max depth for producer-side Redis list (if applicable, more for stream limits).
    int ack_timeout_ms = 30000;                 ///< Timeout (ms) for consumers to ACK messages in reliable modes.
    int max_retries = 3;                        ///< Max retries for NACKed or timed-out messages before moving to DLQ.
    std::string delivery_mode_str = "at-least-once"; ///< String representation of default delivery mode (loaded from config).
    common::DeliveryMode delivery_mode_enum = common::DeliveryMode::AT_LEAST_ONCE; ///< Parsed enum value of default delivery mode.
    int dead_letter_retention_hours = 7 * 24;   ///< How long messages are kept in DLQ before potential cleanup (if implemented).
    bool enable_message_persistence = true;     ///< General flag for enabling message persistence features (ACKs, status).
    int filter_cache_size = 1000;               ///< Placeholder for potential filter result caching.

    /**
     * @brief Parses the `delivery_mode_str` into `delivery_mode_enum`.
     * @details Converts the string representation of delivery mode (case-insensitive)
     *          to the corresponding `common::DeliveryMode` enum value.
     *          If the string is unrecognized, defaults to `AT_LEAST_ONCE` and
     *          a warning should be logged by the caller if appropriate (e.g., during config loading).
     */
    void parseDeliveryMode() {
        std::string lower_mode_str = delivery_mode_str;
        std::transform(lower_mode_str.begin(), lower_mode_str.end(), lower_mode_str.begin(),
                       [](unsigned char c){ return std::tolower(c); });

        if (lower_mode_str == "at-most-once") {
            delivery_mode_enum = common::DeliveryMode::AT_MOST_ONCE;
        } else if (lower_mode_str == "at-least-once") {
            delivery_mode_enum = common::DeliveryMode::AT_LEAST_ONCE;
        } else if (lower_mode_str == "exactly-once") {
            delivery_mode_enum = common::DeliveryMode::EXACTLY_ONCE;
        } else {
            // Logging of warning for unrecognized string should be handled by the config loader
            // as this struct itself doesn't have direct access to SWSS_LOG macros.
            delivery_mode_enum = common::DeliveryMode::AT_LEAST_ONCE; // Default fallback
        }
    }
};
} // namespace swss


#endif // COMMON_ADVANCED_PUBSUB_TYPES_H_
