#ifndef COMMON_ADVANCED_CONSUMER_TABLE_H_
#define COMMON_ADVANCED_CONSUMER_TABLE_H_

#include "common/consumertable.h"
#include "common/advanced_pubsub_types.h" // For common::DeliveryMode, common::Message, swss::PubSubConfig
#include "common/selectable.h" // For swss::Selectable, though ConsumerTable already inherits it
#include "dbconnector.h"     // For swss::DBConnector
#include "configdb.h"        // For swss::ConfigDBConnector
#include "common/pubsub_counters.h" // For PubSubCounters
#include "common/counterstable.h"   // For CountersTable
#include <functional>
#include <string>
#include <vector>
#include <deque>
#include <map>
#include <memory> // For std::shared_ptr, std::unique_ptr
#include <sys/eventfd.h> // For eventfd

// Forward declaration if needed, though common/table.h is included by consumertable.h
// namespace swss { class KeyOpFieldsValuesTuple; }

namespace swss {

/// Default pop batch size for AdvancedConsumerTable if not specified in constructor.
const int DEFAULT_ADVANCED_POP_BATCH_SIZE = 128;

/**
 * @brief Extends ConsumerTable to support advanced Pub/Sub features for message consumption.
 *
 * This class provides functionalities such as:
 * - Message filtering (key-based, content-based, custom predicates).
 * - Manual and automatic message acknowledgment (ACK/NACK).
 * - Dead Letter Queue (DLQ) management for failed messages.
 * - Prioritized message processing (if messages contain priority information).
 * - Integration with swss::Selectable for event-driven consumption.
 * - Metrics collection.
 * - Configuration loading via CONFIG_DB.
 *
 * It maintains an internal queue (`m_priority_queue_out`) for processed and filtered messages
 * that are ready for consumption by the application via its `pops()` methods.
 *
 * @see ConsumerTable
 * @see common::Message
 * @see swss::PubSubConfig
 * @see swss::Selectable
 */
class AdvancedConsumerTable : public ConsumerTable { // ConsumerTable already inherits from Selectable
public:
    /**
     * @brief Constructor for AdvancedConsumerTable.
     * @param db Pointer to the DBConnector instance for message consumption (e.g., APPL_DB).
     * @param tableName The name of the Redis table (stream/list) to consume messages from.
     * @param popBatchSize The default batch size for `pops` operations if not overridden.
     * @param configDb Optional pointer to ConfigDBConnector for loading PubSub configurations.
     *                 If null, default configurations defined in `swss::PubSubConfig` are used.
     */
    AdvancedConsumerTable(DBConnector* db,
                          const std::string& tableName,
                          int popBatchSize = DEFAULT_ADVANCED_POP_BATCH_SIZE,
                          swss::ConfigDBConnector* configDb = nullptr);

    /**
     * @brief Virtual destructor. Cleans up resources like the eventfd.
     */
    virtual ~AdvancedConsumerTable();

    // Filtering Methods
    /**
     * @brief Sets a key-based glob filter.
     * @param glob_pattern The glob pattern to match against message keys.
     * @details Only messages whose `original_key` matches this pattern will be delivered.
     *          Multiple filters can be added; all must pass for a message to be delivered.
     */
    void setKeyFilter(const std::string& glob_pattern);

    /**
     * @brief Sets a content-based JSONPath filter.
     * @param json_path The JSONPath expression to evaluate against the message content.
     * @param expected_value The value expected from the JSONPath evaluation.
     * @details Message content is assumed to be a JSON string. Only messages where the
     *          extracted value matches `expected_value` will be delivered.
     */
    void setContentFilter(const std::string& json_path, const std::string& expected_value);

    /**
     * @brief Sets a custom filter predicate function operating on a `common::Message`.
     * @param predicate A function that takes a `const common::Message&` and returns true if the message should be delivered.
     * @details This allows for complex, application-specific filtering logic.
     *          Currently, only one such custom filter can be set.
     */
    void setCustomFilter(std::function<bool(const common::Message& message)> predicate);

    // Acknowledgment Control
    /**
     * @brief Enables or disables manual message acknowledgment.
     * @param enable If true, manual ACK/NACK is required. If false, messages are auto-acknowledged upon successful pop (behavior depends on delivery mode).
     */
    void enableManualAck(bool enable = true);

    /**
     * @brief Acknowledges a message, indicating successful processing.
     * @param message_id The unique ID of the message to acknowledge.
     * @details If manual acknowledgment is enabled and the message requires it (AT_LEAST_ONCE/EXACTLY_ONCE),
     *          this updates its status in Redis to ACKED and removes it from internal pending lists.
     */
    void ack(const std::string& message_id);

    /**
     * @brief Negatively acknowledges a message, indicating processing failure.
     * @param message_id The unique ID of the message to NACK.
     * @param requeue If true, attempts to requeue the message for another delivery attempt (respecting max_retries).
     * @param error_message An optional error message describing the reason for NACK.
     * @details If manual acknowledgment is enabled:
     *          - For reliable delivery modes, updates status in Redis.
     *          - If requeue is true and retries < max_retries, message status is set to PENDING for re-delivery.
     *          - Otherwise, message status is set to NACKED and moved to DLQ.
     *          - For AT_MOST_ONCE, message is typically moved to DLQ.
     */
    void nack(const std::string& message_id, bool requeue = true, const std::string& error_message = "");

    // Dead Letter Queue (DLQ) Access
    /**
     * @brief Gets a ConsumerTable instance to read messages from the Dead Letter Queue.
     * @return std::unique_ptr<ConsumerTable> A consumer for the DLQ. Ownership is passed to the caller.
     * @details The DLQ name is typically derived from the main table name (e.g., "TABLE_DLQ").
     */
    std::unique_ptr<ConsumerTable> getDeadLetterQueueReader();

    /**
     * @brief Gets the name of the Dead Letter Queue table.
     * @return const std::string& The DLQ table name.
     */
    const std::string& getDlqTableName() const { return m_dlq_table_name; }

    /**
     * @brief Sets a custom name for the Dead Letter Queue table.
     * @param dlq_name The custom name for the DLQ.
     */
    void setDlqTableName(const std::string& dlq_name) { m_dlq_table_name = dlq_name; }


    // Message Retrieval
    /**
     * @brief Pops messages from the consumer, returning them in the KeyOpFieldsValuesTuple format.
     * @param[out] vkco A deque to be filled with popped messages.
     * @param prefix A prefix for keys to pop (functionality might be limited with internal buffering).
     * @details This is an override of the base class method. It internally processes messages
     *          from Redis (fetching, deserializing, filtering) into `m_priority_queue_out`,
     *          then converts `common::Message` objects back to `KeyOpFieldsValuesTuple` for output.
     *          Handles manual ACK tracking if enabled.
     */
    void pops(std::deque<KeyOpFieldsValuesTuple>& vkco, const std::string& prefix = EMPTY_PREFIX) override;

    /**
     * @brief Pops messages from the consumer, returning them as `common::Message` objects.
     * @param[out] messages A deque to be filled with popped `common::Message` objects.
     * @param max_messages_to_pop The maximum number of messages to pop. If 0, uses default batch size.
     * @details This is the preferred method for consuming messages with full metadata.
     *          It processes messages from Redis into `m_priority_queue_out` and then pops them.
     *          Handles manual ACK tracking if enabled.
     */
    void pops(std::deque<common::Message>& messages, size_t max_messages_to_pop = 0);


    // swss::Selectable Interface Implementation
    /**
     * @brief Gets the file descriptor for select operations.
     * @return int The eventfd file descriptor that signals data availability in `m_priority_queue_out`.
     * @details This fd is signaled when `processMessagesFromRedisToOutgoingQueue` adds messages.
     */
    int getFd() override;

    /**
     * @brief Called by Select framework to perform read operation after fd is ready.
     * @return uint64_t Typically 0 for eventfd; actual data is obtained via `pops()`.
     * @details This method clears the eventfd signal by reading from it.
     */
    uint64_t readData() override;

    /**
     * @brief Checks if there is data available for reading from `m_priority_queue_out`.
     * @return true if `m_priority_queue_out` is not empty, false otherwise.
     * @details Reflects the state signaled by `m_event_fd`.
     */
    bool hasData() override;

    /**
     * @brief Checks if there is cached data available (same as `hasData()` for this implementation).
     * @return true if `m_priority_queue_out` is not empty, false otherwise.
     */
    bool hasCachedData() override;

    /**
     * @brief Clears the eventfd after its event has been processed.
     * @details Called by `readData()` or potentially by the `Select` loop after handling.
     */
    void updateAfterRead() override;

    /**
     * @brief Checks if data was available at the time of initialization/registration with Select.
     * @return true if `m_priority_queue_out` is not empty after an initial data processing attempt.
     */
    bool initializedWithData() override;

    /**
     * @brief Processes messages that have timed out waiting for acknowledgment.
     * @details Scans for PENDING messages in Redis that have exceeded `m_config.ack_timeout_ms`.
     *          Attempts to requeue them (respecting `m_config.max_retries`) or moves them to DLQ.
     *          This method should be called periodically by the application.
     */
    void processTimedOutMessages();

    /**
     * @brief Checks if the underlying Redis database connection is responsive.
     * @return true if Redis responds to a PING command, false otherwise.
     */
    bool isRedisConnected() const;

private:
    DBConnector* m_db; ///< Main DBConnector for message consumption and DLQ interaction.
    std::deque<KeyOpFieldsValuesTuple> m_redis_buffer; ///< Internal buffer for raw data from Redis.
    common::PriorityQueue<common::Message> m_priority_queue_out; ///< Queue for processed, filtered messages ready for application.
    std::vector<std::shared_ptr<common::FilterConfig>> m_filters; ///< List of configured filters.
    std::function<bool(const common::Message&)> m_custom_message_filter; ///< Optional custom filter function.

    // Acknowledgment members
    bool m_manual_ack_enabled;      ///< Flag indicating if manual ACK/NACK is enabled.
    DBConnector* m_ack_db_connector; ///< DBConnector for ACK/NACK Redis updates (can be same as m_db).
    std::map<std::string, common::Message> m_pending_ack_messages; ///< Stores messages delivered but awaiting ACK/NACK.

    // DLQ members
    std::string m_dlq_table_name;                   ///< Name of the Dead Letter Queue table.
    std::unique_ptr<ProducerTable> m_dlq_producer;  ///< Producer instance to write messages to the DLQ.

    // Configuration
    swss::PubSubConfig m_config; ///< Holds configuration like max_retries, ack_timeout_ms.

    // Selectable integration
    int m_event_fd; ///< Event FD to signal availability of messages in `m_priority_queue_out`.

    // Counters
    std::unique_ptr<swss::CountersTable> m_counters_table; ///< Table for Pub/Sub metrics.

    // Private Helper Methods
    /** @brief Applies all configured filters to a message. */
    bool applyFilters(const common::Message& msg);
    /** @brief Moves a message to the Dead Letter Queue. */
    void moveToDLQ(common::Message& msg, const std::string& reason);
    /** @brief Fetches from Redis, deserializes, filters, and enqueues messages to `m_priority_queue_out`. */
    void processMessagesFromRedisToOutgoingQueue(size_t max_to_process = 0);
    /** @brief Deserializes raw Redis data (KeyOpFieldsValuesTuple) into a common::Message. */
    bool deserializeMessage(const KeyOpFieldsValuesTuple& kco, common::Message& message);
    /** @brief Serializes a common::Message back to KeyOpFieldsValuesTuple (for pops(vkco) or other uses). */
    KeyOpFieldsValuesTuple serializeMessageToKCO(const common::Message& message) const;
    /** @brief Ensures the DLQ producer is initialized. */
    void ensureDlqProducer();
    /** @brief Reconstructs a minimal common::Message from its status hash in Redis (for timeout/DLQ). */
    bool reconstructMessageFromStatus(const std::string& message_id, common::Message& message);

    // DLQ Management
    /** @brief (Placeholder) Trims old messages from the Dead Letter Queue based on retention policy. */
    void trimDeadLetterQueue();
    /** @brief (Placeholder) Pops messages from DLQ for potential replay. */
    std::vector<common::Message> popMessagesForReplay(size_t count = 1);
};

} // namespace swss

#endif // COMMON_ADVANCED_CONSUMER_TABLE_H_
