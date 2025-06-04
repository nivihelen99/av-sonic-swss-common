#ifndef COMMON_ADVANCED_CONSUMER_TABLE_H_
#define COMMON_ADVANCED_CONSUMER_TABLE_H_

#include "common/consumertable.h"
#include "common/advanced_pubsub_types.h"
#include "common/selectable.h" // For swss::Selectable, though ConsumerTable already inherits it
#include "dbconnector.h"     // For swss::DBConnector
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

// Default pop batch size, can be overridden in constructor
const int DEFAULT_ADVANCED_POP_BATCH_SIZE = 128;

class AdvancedConsumerTable : public ConsumerTable {
public:
    // Constructors mirroring swss::ConsumerTable
    AdvancedConsumerTable(DBConnector* db, const std::string& tableName, int popBatchSize = DEFAULT_ADVANCED_POP_BATCH_SIZE)
        : ConsumerTable(db, tableName, popBatchSize),
          m_manual_ack_enabled(false),
          m_dlq_table_name(tableName + "_DLQ") // Default DLQ name
          // m_event_fd(-1) // Initialize event_fd if used directly
          {}

    // Note: ConsumerTable also has a constructor with a timeout.
    // ConsumerTable(DBConnector *db, const std::string &tableName, int popBatchSize, int pri, int timeout);
    // We might want to mirror that as well if high-priority channel access is needed directly.
    // For now, focusing on the primary constructor.

    virtual ~AdvancedConsumerTable();

    // Filtering Methods
    void setKeyFilter(const std::string& glob_pattern);
    void setContentFilter(const std::string& json_path, const std::string& expected_value);
    void setCustomFilter(std::function<bool(const common::Message& message)> predicate); // Takes Message for more context

    // Acknowledgment Control
    void enableManualAck(bool enable = true);
    void ack(const std::string& message_id);
    void nack(const std::string& message_id, bool requeue_payload_to_original_topic = true, const std::string& new_error_message = "");

    // Dead Letter Queue (DLQ) Access
    // Returns a ConsumerTable to read from the DLQ. Ownership is passed to the caller.
    std::unique_ptr<ConsumerTable> getDeadLetterQueueReader();
    const std::string& getDlqTableName() const { return m_dlq_table_name; }
    void setDlqTableName(const std::string& dlq_name) { m_dlq_table_name = dlq_name; }


    // Message Retrieval
    // Overrides pops from ConsumerTableBase (via ConsumerTable)
    // The prefix parameter might not be directly applicable if we are pulling into a priority queue first.
    void pops(std::deque<KeyOpFieldsValuesTuple>& vkco, const std::string& prefix = EMPTY_PREFIX) override;

    // Alternative pop that returns structured Messages, more aligned with advanced features
    void pops(std::deque<common::Message>& messages, size_t max_messages = 0);


    // swss::Selectable Implementation (ConsumerTable already inherits Selectable)
    // We override these to reflect the state of our internal priority queue or filtered messages.
    int getFd() override;
    uint64_t readData() override; // Clears eventfd, returns 0 or items in queue
    bool hasData() override; // Checks m_priority_queue_out
    bool hasCachedData() override; // Same as hasData for this implementation
    void updateAfterRead() override; // Clears eventfd
    bool initializedWithData() override; // Checks m_priority_queue_out after initial pull

    // Method to process messages that have timed out waiting for ACK
    void processTimedOutMessages();

private:
    DBConnector* m_db; // Store DBConnector for DLQ producer, etc.
    // Internal buffer for messages pulled from Redis, pre-filtering/processing
    std::deque<KeyOpFieldsValuesTuple> m_redis_buffer;

    // Priority queue for messages that are ready for application consumption (post-filtering)
    common::PriorityQueue<common::Message> m_priority_queue_out;

    // Filter configurations
    std::vector<std::shared_ptr<common::FilterConfig>> m_filters;
    // For custom filter that needs more than just KeyOpFieldsValuesTuple
    std::function<bool(const common::Message&)> m_custom_message_filter;


    // Acknowledgment members
    bool m_manual_ack_enabled;
    DBConnector* m_ack_db_connector; // DB connector for ACK/NACK updates in Redis (can be same as m_db)
    std::map<std::string, common::Message> m_pending_ack_messages; // Stores messages delivered but not yet acked/nacked

    // DLQ members
    std::string m_dlq_table_name;
    std::unique_ptr<ProducerTable> m_dlq_producer; // To send messages to DLQ

    // Configuration
    swss::PubSubConfig m_config; // Holds configuration like max_retries, ack_timeout_ms

    // Selectable integration
    int m_event_fd; // Event FD to signal availability of messages in m_priority_queue_out

    // Private Helper Methods
    bool applyFilters(const common::Message& msg); // Returns true if message should be delivered
    void moveToDLQ(const common::Message& msg, const std::string& error_reason);

    // Main internal processing loop to fetch from Redis, deserialize, filter, and enqueue to m_priority_queue_out
    void processMessagesFromRedisToOutgoingQueue(size_t max_to_process = 0);

    // Helper to deserialize KeyOpFieldsValuesTuple to common::Message
    bool deserializeMessage(const KeyOpFieldsValuesTuple& kco, common::Message& message);

    // Helper to convert common::Message back to KeyOpFieldsValuesTuple for DLQ or requeue
    KeyOpFieldsValuesTuple serializeMessageToKCO(const common::Message& message) const;

    // Helper to initialize DLQ producer
    void ensureDlqProducer();

    // Helper to reconstruct message from status hash (minimal for DLQ or timeout processing)
    bool reconstructMessageFromStatus(const std::string& message_id, common::Message& message);
};

} // namespace swss

#endif // COMMON_ADVANCED_CONSUMER_TABLE_H_
