#ifndef COMMON_ADVANCED_PRODUCER_TABLE_H_
#define COMMON_ADVANCED_PRODUCER_TABLE_H_

#include "common/producertable.h"
#include "common/advanced_pubsub_types.h" // For common::DeliveryMode, common::Message, swss::PubSubConfig
#include "dbconnector.h" // For DBConnector
#include "configdb.h"    // For swss::ConfigDBConnector
#include "common/pubsub_counters.h" // For PubSubCounters
#include "common/counterstable.h"   // For CountersTable
#include <string>
#include <vector>
#include <map> // May be needed later for ack tracking
#include <memory> // For std::unique_ptr

namespace swss {

/**
 * @brief Extends ProducerTable to support advanced Pub/Sub features.
 *
 * This class provides functionalities like message prioritization, configurable delivery modes,
 * acknowledgment tracking for reliable messaging, and metrics collection.
 * It uses an internal priority queue to manage messages before flushing them to Redis.
 * Configuration can be loaded from CONFIG_DB.
 *
 * @see ProducerTable
 * @see common::Message
 * @see swss::PubSubConfig
 */
class AdvancedProducerTable : public ProducerTable {
public:
    /**
     * @brief Constructor for AdvancedProducerTable using DBConnector.
     * @param db Pointer to the DBConnector instance for message production (e.g., APPL_DB).
     * @param tableName The name of the Redis table (stream/list) to produce messages to.
     * @param configDb Optional pointer to ConfigDBConnector for loading PubSub configurations.
     *                 If null, default configurations are used.
     */
    AdvancedProducerTable(DBConnector* db,
                          const std::string& tableName,
                          swss::ConfigDBConnector* configDb = nullptr);

    /**
     * @brief Constructor for AdvancedProducerTable using RedisPipeline.
     * @param pipeline Pointer to the RedisPipeline instance for message production.
     * @param tableName The name of the Redis table (stream/list) to produce messages to.
     * @param buffered Whether operations through this pipeline are buffered.
     * @param configDb Optional pointer to ConfigDBConnector for loading PubSub configurations.
     *                 If null, default configurations are used.
     * @note If using RedisPipeline, ACK tracking features that require direct DBConnector access
     *       (m_ack_db_connector) might be disabled if not set up separately. Counters also depend on m_db.
     */
    AdvancedProducerTable(RedisPipeline* pipeline,
                          const std::string& tableName,
                          bool buffered = false,
                          swss::ConfigDBConnector* configDb = nullptr);

    /**
     * @brief Enqueues a message with specified key, values, priority, and correlation ID.
     * @param key The key for the message.
     * @param values A vector of FieldValueTuple representing the message data.
     * @param priority The message priority (0-9, 9 is highest). Defaults to a value from PubSubConfig or 5.
     * @param correlation_id Optional ID for tracking a group of messages or a batch.
     * @details This method creates a `common::Message` object, populates it, and pushes it
     *          onto an internal priority queue. Messages are actually sent to Redis when
     *          `flushPriorityQueue()` is called. The default delivery mode set by `setDeliveryMode()`
     *          or loaded from config is used for the message.
     */
    void set(const std::string& key, const std::vector<FieldValueTuple>& values,
             int priority, // Default is m_config.default_priority, but C++ requires default in declaration or one definition
             const std::string& correlation_id = "");

    // Make base class set methods available
    using ProducerTable::set;

    /**
     * @brief Sets the default delivery mode for messages sent by this producer instance.
     * @param mode The desired common::DeliveryMode.
     * @details This delivery mode will be applied to messages created via the `set` method
     *          unless overridden by message-specific configurations (not currently implemented).
     */
    void setDeliveryMode(common::DeliveryMode mode);

    /**
     * @brief Waits for acknowledgment of all messages associated with a given correlation ID.
     * @param correlation_id The correlation ID to wait for.
     * @param timeout_ms The maximum time in milliseconds to wait for acknowledgments.
     * @return true if all messages for the correlation ID are ACKed within the timeout.
     * @return false if a timeout occurs, any message is NACKed, or an error happens.
     * @details This method polls Redis for the status of messages. It requires ACK tracking
     *          to be enabled (via m_ack_db_connector) and for messages to have been sent
     *          with AT_LEAST_ONCE or EXACTLY_ONCE delivery mode.
     */
    bool waitForAck(const std::string& correlation_id, int timeout_ms);

    /**
     * @brief Flushes all messages from the internal priority queue to Redis.
     * @details Messages are popped from the priority queue in order of priority (and then FIFO)
     *          and sent to Redis using the underlying `ProducerTable::set` method.
     *          If ACK tracking is enabled for a message, its status is recorded in Redis.
     */
    void flushPriorityQueue();

    /**
     * @brief Checks if the underlying Redis database connection is responsive.
     * @return true if Redis responds to a PING command, false otherwise.
     */
    bool isRedisConnected() const;

    /**
     * @brief Default virtual destructor.
     */
    virtual ~AdvancedProducerTable() = default;

private:
    DBConnector* m_db; ///< Main DBConnector for producing messages.
    DBConnector* m_ack_db_connector; ///< DBConnector for ACK tracking (can be same as m_db).
    common::DeliveryMode m_delivery_mode; ///< Default delivery mode for messages from this producer.
    common::PriorityQueue<common::Message> m_priority_queue; ///< Internal priority queue for messages.
    swss::PubSubConfig m_config; ///< PubSub configuration settings.
    std::unique_ptr<swss::CountersTable> m_counters_table; ///< Table for Pub/Sub metrics.
    std::string m_counters_producer_queue_depth_key; ///< Cached Redis key for producer queue depth counter.

    // Placeholder for direct client-side ACK tracking if needed beyond Redis.
    // std::map<std::string, SomeAckTrackingInfo> m_ack_tracker;

    // TODO: Implement queue overflow handling mechanism
    // e.g., drop oldest, drop lowest priority, block producer, etc.

    // TODO: Implement backpressure mechanism if the queue grows too large
};

} // namespace swss

#endif // COMMON_ADVANCED_PRODUCER_TABLE_H_
