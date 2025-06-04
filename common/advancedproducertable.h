#ifndef COMMON_ADVANCED_PRODUCER_TABLE_H_
#define COMMON_ADVANCED_PRODUCER_TABLE_H_

#include "common/producertable.h"
#include "common/advanced_pubsub_types.h" // For common::DeliveryMode, common::Message, swss::PubSubConfig
#include "dbconnector.h" // For DBConnector
#include "configdb.h"    // For swss::ConfigDBConnector
#include "common/pubsub_counters.h" // For PubSubCounters
#include "common/counterstable.h"   // For CountersTable
#include "common/selectable.h"      // For swss::Selectable
#include "common/redisselect.h"     // For swss::RedisSelect
#include <string>
#include <vector>
#include <map>
#include <set>     // For std::set
#include <memory>  // For std::unique_ptr
#include <mutex>   // For std::mutex
#include <optional> // For std::optional

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
 * @see swss::Selectable
 * @brief Extends ProducerTable to support advanced Pub/Sub features including message prioritization,
 *        reliable delivery with ACK/NACK, configurable delivery modes, and asynchronous ACK notifications.
 *        It can be monitored via `select()` for asynchronous ACK/NACK group status notifications.
 */
class AdvancedProducerTable : public ProducerTable, public Selectable {
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
    virtual ~AdvancedProducerTable();

    // Asynchronous ACK notification methods
    /**
     * @brief Registers a correlation ID for asynchronous acknowledgment notification.
     * @param correlation_id The correlation ID to monitor.
     * @details The producer will subscribe to a Redis Pub/Sub channel for this correlation ID
     *          to receive notifications when all messages in the group are ACKed or if one is NACKed.
     */
    void registerCorrelationForAsyncAck(const std::string& correlation_id);

    /**
     * @brief Unregisters a correlation ID from asynchronous acknowledgment monitoring.
     * @param correlation_id The correlation ID to unregister.
     * @param force_unsubscribe If true, also clears any stored async ACK result for this ID.
     * @details Unsubscribes from the Redis Pub/Sub channel and cleans up internal tracking.
     *          Should be called if the application no longer needs to wait for or check the async result,
     *          or if `waitForAck` (which uses this mechanism internally) times out or completes.
     *          `processAsyncAckNotifications` also calls this internally when a final notification is processed.
     */
    void unregisterCorrelationForAsyncAck(const std::string& correlation_id, bool force_unsubscribe = false);

    /**
     * @brief Retrieves the final asynchronous acknowledgment result for a correlation ID.
     * @param correlation_id The correlation ID to check.
     * @param remove_on_get If true (default), the result is removed from the internal store after retrieval,
     *                      and the producer unsubscribes from further notifications for this correlation ID.
     * @return std::optional<FinalAckResult> The result if a final notification has been processed, otherwise `std::nullopt`.
     * @details This method allows polling for the result after `registerCorrelationForAsyncAck` has been called
     *          and notifications are being processed (typically via `processAsyncAckNotifications` being triggered by `select()`).
     */
    std::optional<FinalAckResult> getAsyncAckResult(const std::string& correlation_id, bool remove_on_get = true);

    /**
     * @brief Processes incoming asynchronous ACK notifications from subscribed Redis channels.
     * @details This method should be called when the producer's file descriptor (obtained via `getFd()`)
     *          is reported as ready by `select()`, typically by calling `readData()` on this Selectable object.
     *          It reads messages from the notification channels, parses them, stores the final results,
     *          and unregisters/unsubscribes the corresponding correlation IDs.
     */
    void processAsyncAckNotifications();

    // Selectable interface overrides
    /**
     * @brief Gets the file descriptor for asynchronous ACK notifications.
     * @return int The file descriptor provided by `RedisSelect` for Pub/Sub. Use with `swss::Select`.
     * @details This FD becomes readable when an ACK/NACK group notification is published on a channel
     *          this producer instance is subscribed to via `registerCorrelationForAsyncAck`.
     */
    int getFd() override;

    /**
     * @brief Processes asynchronous ACK notifications when the FD from `getFd()` is ready.
     * @return uint64_t Typically 0. The return value is not significant for event-style notifications.
     * @details This method calls `processAsyncAckNotifications()` to handle the incoming data.
     */
    uint64_t readData() override;

    /**
     * @brief Checks if there are pending asynchronous ACK notifications to be processed.
     * @return true if `m_ack_notifier_select` has data from Pub/Sub channels, false otherwise.
     */
    bool hasData() override;

    /**
     * @brief Checks for cached/pending data (same as `hasData()` for this implementation).
     * @return true if `m_ack_notifier_select` has data, false otherwise.
     */
    bool hasCachedData() override;

    /**
     * @brief Performs operations after `readData()` has processed notifications.
     * @details Currently a no-op as `RedisSelect::pop` (used in `processAsyncAckNotifications`)
     *          manages its internal state. If `RedisSelect` required manual state update after reads,
     *          it would be done here.
     */
    void updateAfterRead() override;

    /**
     * @brief Checks if notifications were available upon initialization.
     * @return bool Always false for this implementation, as async ACKs are responses to produced messages.
     */
    bool initializedWithData() override;

private:
    DBConnector* m_db; ///< Main DBConnector for producing messages.
    DBConnector* m_ack_db_connector; ///< DBConnector for ACK tracking and Pub/Sub for async ACKs.
    common::DeliveryMode m_delivery_mode; ///< Default delivery mode for messages from this producer.
    common::PriorityQueue<common::Message> m_priority_queue; ///< Internal priority queue for messages.
    swss::PubSubConfig m_config; ///< PubSub configuration settings.
    std::unique_ptr<swss::CountersTable> m_counters_table; ///< Table for Pub/Sub metrics.
    std::string m_counters_producer_queue_depth_key; ///< Cached Redis key for producer queue depth counter.

    // Asynchronous ACK members
    std::unique_ptr<swss::RedisSelect> m_ack_notifier_select; ///< RedisSelect for subscribing to ACK notification channels.
    std::set<std::string> m_pending_async_corr_ids;           ///< Correlation IDs being monitored for async ACKs.
    std::map<std::string, FinalAckResult> m_async_ack_results;  ///< Stores final ACK results for correlation IDs.
    mutable std::mutex m_async_ack_mutex;                     ///< Mutex to protect m_pending_async_corr_ids and m_async_ack_results.

    // TODO: Implement queue overflow handling mechanism
    // e.g., drop oldest, drop lowest priority, block producer, etc.

    // TODO: Implement backpressure mechanism if the queue grows too large
};

} // namespace swss

#endif // COMMON_ADVANCED_PRODUCER_TABLE_H_
