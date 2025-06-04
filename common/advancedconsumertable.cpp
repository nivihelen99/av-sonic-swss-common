#include "common/advancedconsumertable.h"
#include "common/json.h"
#include "common/logger.h"
#include "common/table.h"     // For FieldValueTuple, though often included via others
#include "common/redisapi.h"  // For various Redis constants or utilities if needed directly
#include "common/producertable.h" // For m_dlq_producer

#include <vector>
#include <utility> // For std::pair
#include <chrono>  // For std::chrono types
#include <sys/eventfd.h> // For eventfd
#include <unistd.h> // For close()
#include <cerrno>   // For errno
#include <cstring>  // For strerror

// Helper to deserialize swss::Json to common::Message
// Similar to to_json, this should ideally be in a common utility or alongside swss::Json definitions.
namespace swss {
    // Assuming swss::Json is similar to nlohmann::json
    void from_json(const Json& j, common::Message& m) {
        j.at("id").get_to(m.id);
        j.at("priority").get_to(m.priority);
        // original_key in the JSON is the one set by the producer.
        // The consumer will overwrite message.original_key with the actual key it read from Redis.
        j.at("original_key").get_to(m.original_key);
        j.at("content").get_to(m.content); // content is already a JSON string of original FieldValueTuples
        long long timestamp_ms;
        j.at("timestamp_ms").get_to(timestamp_ms);
        m.timestamp = std::chrono::time_point<std::chrono::system_clock>(std::chrono::milliseconds(timestamp_ms));
        j.at("retry_count").get_to(m.retry_count);
        j.at("correlation_id").get_to(m.correlation_id);
        int delivery_mode_int;
        j.at("delivery_mode").get_to(delivery_mode_int);
        m.delivery_mode = static_cast<common::DeliveryMode>(delivery_mode_int);
    }
} // namespace swss


namespace swss {

// Constructor
AdvancedConsumerTable::AdvancedConsumerTable(DBConnector* db, const std::string& tableName, int popBatchSize)
    : ConsumerTable(db, tableName, popBatchSize),
      m_db(db),
      m_ack_db_connector(db), // Initialize m_ack_db_connector
      // m_max_retries is now part of m_config, which is default-initialized.
      // m_config.max_retries will be used directly.
      m_manual_ack_enabled(false),
      m_dlq_table_name(tableName + "_DLQ"),
      m_event_fd(-1) {
    // m_config uses its default constructor. Specific values can be set here if needed,
    // e.g., m_config.max_retries = 5; or loaded from a config file later.
    // For now, we rely on defaults set in PubSubConfig struct definition.
    m_event_fd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    if (m_event_fd == -1) {
        SWSS_LOG_ERROR("AdvancedConsumerTable ('%s'): Failed to create eventfd: %s",
                       tableName.c_str(), strerror(errno));
        throw std::runtime_error("Failed to create eventfd for AdvancedConsumerTable: " + std::string(strerror(errno)));
    }
    SWSS_LOG_INFO("AdvancedConsumerTable: Initialized for table '%s', DLQ table '%s', event_fd %d",
                  tableName.c_str(), m_dlq_table_name.c_str(), m_event_fd);
}

// Destructor
AdvancedConsumerTable::~AdvancedConsumerTable() {
    if (m_event_fd != -1) {
        close(m_event_fd);
        m_event_fd = -1;
    }
    SWSS_LOG_INFO("AdvancedConsumerTable: Destructor for table '%s'", getTableName().c_str());
}

// --- Filtering Methods ---
void AdvancedConsumerTable::setKeyFilter(const std::string& glob_pattern) {
    auto filter_config = std::make_shared<common::KeyGlobFilterConfig>();
    filter_config->filter_id = "key_glob_" + std::to_string(m_filters.size());
    filter_config->glob_pattern = glob_pattern;
    m_filters.push_back(filter_config);
    SWSS_LOG_INFO("AdvancedConsumerTable ('%s'): Added key glob filter: '%s'", getTableName().c_str(), glob_pattern.c_str());
}

void AdvancedConsumerTable::setContentFilter(const std::string& json_path, const std::string& expected_value) {
    auto filter_config = std::make_shared<common::JsonPathFilterConfig>();
    filter_config->filter_id = "json_path_" + std::to_string(m_filters.size());
    filter_config->json_path_expression = json_path;
    filter_config->expected_value = expected_value;
    m_filters.push_back(filter_config);
    SWSS_LOG_INFO("AdvancedConsumerTable ('%s'): Added content JSON path filter: path='%s', value='%s'",
                  getTableName().c_str(), json_path.c_str(), expected_value.c_str());
}

void AdvancedConsumerTable::setCustomFilter(std::function<bool(const common::Message&)> predicate) {
    m_custom_message_filter = predicate;
    SWSS_LOG_INFO("AdvancedConsumerTable ('%s'): Set custom message filter.", getTableName().c_str());
}

// --- Acknowledgment Control ---
void AdvancedConsumerTable::enableManualAck(bool enable) {
    m_manual_ack_enabled = enable;
    SWSS_LOG_INFO("AdvancedConsumerTable ('%s'): Manual acknowledgment %s.",
                  getTableName().c_str(), enable ? "enabled" : "disabled");
}

void AdvancedConsumerTable::ack(const std::string& message_id) {
    if (!m_manual_ack_enabled) {
        SWSS_LOG_WARN("AdvancedConsumerTable ('%s'): ack() called but manual acknowledgment is disabled. Message ID: '%s'",
                      getTableName().c_str(), message_id.c_str());
        return;
    }
    auto it = m_pending_ack_messages.find(message_id);
    if (it == m_pending_ack_messages.end()) {
        SWSS_LOG_WARN("AdvancedConsumerTable ('%s'): ack() called for unknown or already processed Message ID '%s'.",
                      getTableName().c_str(), message_id.c_str());
        return;
    }

    const common::Message& acked_message = it->second;

    if (m_ack_db_connector &&
        (acked_message.delivery_mode == common::DeliveryMode::AT_LEAST_ONCE ||
         acked_message.delivery_mode == common::DeliveryMode::EXACTLY_ONCE)) {

        std::string status_key = std::string(common::Message::MSG_STATUS_PREFIX) + acked_message.id;
        try {
            if (m_ack_db_connector->getRedisContext()->exists({status_key})[0]) {
                std::vector<std::pair<std::string, std::string>> status_update_fields;
                status_update_fields.emplace_back(common::Message::MSG_STAT_FIELD_STATUS, "ACKED");
                status_update_fields.emplace_back(common::Message::MSG_STAT_FIELD_TIMESTAMP,
                                                  std::to_string(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count()));
                m_ack_db_connector->getRedisContext()->hmset(status_key, status_update_fields.begin(), status_update_fields.end());
                // ACKED messages can expire from status table after a while (e.g. dead_letter_retention_hours).
                m_ack_db_connector->getRedisContext()->expire(status_key, m_config.dead_letter_retention_hours * 3600);

                SWSS_LOG_INFO("AdvancedConsumerTable ('%s'): Message ID '%s' status updated to ACKED in Redis.",
                              getTableName().c_str(), acked_message.id.c_str());

                // Remove from PENDING_MESSAGES_BY_TIME sorted set
                m_ack_db_connector->getRedisContext()->zrem(common::Message::PENDING_MESSAGES_BY_TIME_KEY, { acked_message.id });

                if (!acked_message.correlation_id.empty()) {
                    std::string corr_key = std::string(common::Message::CORRELATION_IDS_PREFIX) + acked_message.correlation_id;
                    m_ack_db_connector->getRedisContext()->srem(corr_key, { acked_message.id });
                    // Optionally, if the set becomes empty, the correlation ID key itself could be deleted or expired.
                    // This depends on whether the producer might add more messages to the same correlation ID later.
                    // For now, let the correlation ID key expire as set by producer.
                }
            } else {
                SWSS_LOG_WARN("AdvancedConsumerTable ('%s'): Status key for Message ID '%s' not found in Redis for ACK. Possibly expired or already processed.",
                              getTableName().c_str(), acked_message.id.c_str());
            }
        } catch (const std::exception& e) {
            SWSS_LOG_ERROR("AdvancedConsumerTable ('%s'): Exception updating status to ACKED for Message ID '%s': %s",
                           getTableName().c_str(), acked_message.id.c_str(), e.what());
        }
    }

    m_pending_ack_messages.erase(it);
    SWSS_LOG_DEBUG("AdvancedConsumerTable ('%s'): Message ID '%s' processed as ACKED locally.", getTableName().c_str(), message_id.c_str());
}

void AdvancedConsumerTable::nack(const std::string& message_id, bool requeue, const std::string& error_message) {
    if (message_id.empty()) {
        SWSS_LOG_ERROR("AdvancedConsumerTable ('%s'): nack() called with empty message_id.", getTableName().c_str());
        return;
    }
    if (!m_manual_ack_enabled) {
        SWSS_LOG_WARN("AdvancedConsumerTable ('%s'): nack() called but manual acknowledgment is disabled. Message ID: '%s'",
                      getTableName().c_str(), message_id.c_str());
        return;
    }

    auto it = m_pending_ack_messages.find(message_id);
    if (it == m_pending_ack_messages.end()) {
        SWSS_LOG_WARN("AdvancedConsumerTable ('%s'): nack() called for unknown or already processed Message ID '%s'.",
                      getTableName().c_str(), message_id.c_str());
        return;
    }

    common::Message nacked_message = it->second;

    SWSS_LOG_INFO("AdvancedConsumerTable ('%s'): Message ID '%s' NACKed by client. Error: '%s'. Requeue: %s.",
                  getTableName().c_str(), message_id.c_str(), error_message.c_str(), requeue ? "true" : "false");

    bool remove_from_pending_map = true;

    if (m_ack_db_connector &&
        (nacked_message.delivery_mode == common::DeliveryMode::AT_LEAST_ONCE ||
         nacked_message.delivery_mode == common::DeliveryMode::EXACTLY_ONCE)) {

        std::string status_key = std::string(common::Message::MSG_STATUS_PREFIX) + nacked_message.id;
        long long current_ts_ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();

        if (requeue && nacked_message.retry_count < m_config.max_retries) {
            nacked_message.retry_count++;
            std::vector<std::pair<std::string, std::string>> status_fields_update;
            status_fields_update.emplace_back(common::Message::MSG_STAT_FIELD_STATUS, "PENDING");
            status_fields_update.emplace_back(common::Message::MSG_STAT_FIELD_TIMESTAMP, std::to_string(current_ts_ms));
            status_fields_update.emplace_back("last_nack_reason", error_message);
            status_fields_update.emplace_back("retry_count", std::to_string(nacked_message.retry_count));

            try {
                 if (m_ack_db_connector->getRedisContext()->exists({status_key})[0]) {
                    m_ack_db_connector->getRedisContext()->hmset(status_key, status_fields_update.begin(), status_fields_update.end());
                    // Update expiry to allow time for re-delivery
                    m_ack_db_connector->getRedisContext()->expire(status_key, m_config.ack_timeout_ms / 1000 + 60); // Give it time to be reprocessed

                    // Update score in PENDING_MESSAGES_BY_TIME sorted set to new timestamp for re-timeout
                    m_ack_db_connector->getRedisContext()->zadd(
                        common::Message::PENDING_MESSAGES_BY_TIME_KEY,
                        static_cast<double>(current_ts_ms),
                        nacked_message.id
                    );
                    SWSS_LOG_INFO("AdvancedConsumerTable ('%s'): Message ID '%s' status updated to PENDING for requeue (Retry %d/%d). Error: %s",
                                  getTableName().c_str(), nacked_message.id.c_str(), nacked_message.retry_count, m_config.max_retries, error_message.c_str());
                 } else {
                     SWSS_LOG_WARN("AdvancedConsumerTable ('%s'): Status key for Message ID '%s' not found for requeue. Moving to DLQ.",
                                   getTableName().c_str(), nacked_message.id.c_str());
                     moveToDLQ(nacked_message, "NACKed_StatusKeyMissing_CannotRequeue: " + error_message);
                     m_ack_db_connector->getRedisContext()->zrem(common::Message::PENDING_MESSAGES_BY_TIME_KEY, { nacked_message.id });
                 }
            } catch (const std::exception& e) {
                SWSS_LOG_ERROR("AdvancedConsumerTable ('%s'): Exception updating status to PENDING for Message ID '%s' for requeue: %s. Not removing from local pending map.",
                               getTableName().c_str(), nacked_message.id.c_str(), e.what());
                remove_from_pending_map = false;
            }
        } else { // Not requeuing (max retries exceeded or requeue=false)
            try {
                std::vector<std::pair<std::string, std::string>> status_fields_nack;
                status_fields_nack.emplace_back(common::Message::MSG_STAT_FIELD_STATUS, "NACKED");
                status_fields_nack.emplace_back("nack_reason", error_message);
                status_fields_nack.emplace_back(common::Message::MSG_STAT_FIELD_TIMESTAMP, std::to_string(current_ts_ms));
                status_fields_nack.emplace_back("retry_count", std::to_string(nacked_message.retry_count));

                if (m_ack_db_connector->getRedisContext()->exists({status_key})[0]) {
                     m_ack_db_connector->getRedisContext()->hmset(status_key, status_fields_nack.begin(), status_fields_nack.end());
                     m_ack_db_connector->getRedisContext()->expire(status_key, m_config.dead_letter_retention_hours * 3600);
                } else {
                     SWSS_LOG_WARN("AdvancedConsumerTable ('%s'): Status key for Message ID '%s' not found during final NACK. It might have expired.",
                                   getTableName().c_str(), nacked_message.id.c_str());
                }

                m_ack_db_connector->getRedisContext()->zrem(common::Message::PENDING_MESSAGES_BY_TIME_KEY, { nacked_message.id });
                if (!nacked_message.correlation_id.empty()) {
                    std::string corr_key = std::string(common::Message::CORRELATION_IDS_PREFIX) + nacked_message.correlation_id;
                    m_ack_db_connector->getRedisContext()->srem(corr_key, { nacked_message.id });
                }
            } catch (const std::exception& e) {
                 SWSS_LOG_ERROR("AdvancedConsumerTable ('%s'): Exception updating status to NACKED for Message ID '%s': %s. Not removing from local pending map.",
                               getTableName().c_str(), nacked_message.id.c_str(), e.what());
                 remove_from_pending_map = false;
            }
            SWSS_LOG_INFO("AdvancedConsumerTable ('%s'): Message ID '%s' NACKed. Max retries (%d) exceeded or requeue disabled. Error: %s. Moving to DLQ.",
                          getTableName().c_str(), nacked_message.id.c_str(), nacked_message.retry_count, error_message.c_str());
            moveToDLQ(nacked_message, "NACKed_FinalNoRequeue: " + error_message);
        }
    } else {
        SWSS_LOG_INFO("AdvancedConsumerTable ('%s'): Message ID '%s' (mode: AT_MOST_ONCE) NACKed by client. Error: '%s'. Moving to DLQ.",
                      getTableName().c_str(), message_id.c_str(), error_message.c_str());
        moveToDLQ(nacked_message, "NACKed_AtMostOnce: " + error_message);
    }

    if (remove_from_pending_map) {
        m_pending_ack_messages.erase(it);
    }
}

// --- DLQ Access Methods ---
std::unique_ptr<ConsumerTable> AdvancedConsumerTable::getDeadLetterQueueReader() {
    SWSS_LOG_INFO("AdvancedConsumerTable ('%s'): Creating DLQ reader for table '%s'.",
                  getTableName().c_str(), m_dlq_table_name.c_str());
    // The caller owns the DBConnector, so it should remain valid.
    return std::make_unique<ConsumerTable>(m_db, m_dlq_table_name, getPopBatchSize());
}

// getDlqTableName and setDlqTableName are inline in header or trivial

// --- Private Helper: DLQ Producer ---
void AdvancedConsumerTable::ensureDlqProducer() {
    if (!m_dlq_producer) {
        SWSS_LOG_INFO("AdvancedConsumerTable ('%s'): Initializing DLQ producer for table '%s'.",
                      getTableName().c_str(), m_dlq_table_name.c_str());
        m_dlq_producer = std::make_unique<ProducerTable>(m_db, m_dlq_table_name);
    }
}

void AdvancedConsumerTable::moveToDLQ(const common::Message& msg, const std::string& error_reason) {
    ensureDlqProducer();
    if (!m_dlq_producer) {
        SWSS_LOG_ERROR("AdvancedConsumerTable ('%s'): Failed to initialize DLQ producer. Cannot move Message ID '%s' to DLQ.",
                       getTableName().c_str(), msg.id.c_str());
        return;
    }

    SWSS_LOG_WARN("AdvancedConsumerTable ('%s'): Moving Message ID '%s' (Original Key: '%s') to DLQ '%s'. Reason: %s",
                  getTableName().c_str(), msg.id.c_str(), msg.original_key.c_str(), m_dlq_table_name.c_str(), error_reason.c_str());

    std::vector<FieldValueTuple> fv_dlq;
    fv_dlq.emplace_back("original_id", msg.id);
    fv_dlq.emplace_back("original_key", msg.original_key);
    fv_dlq.emplace_back("original_content", msg.content); // The already serialized content
    fv_dlq.emplace_back("original_correlation_id", msg.correlation_id);
    fv_dlq.emplace_back("original_priority", std::to_string(msg.priority));
    fv_dlq.emplace_back("original_timestamp", std::to_string(std::chrono::duration_cast<std::chrono::milliseconds>(msg.timestamp.time_since_epoch()).count()));
    fv_dlq.emplace_back("original_delivery_mode", std::to_string(static_cast<int>(msg.delivery_mode)));
    fv_dlq.emplace_back("retry_count", std::to_string(msg.retry_count));
    fv_dlq.emplace_back("dlq_reason", error_reason);
    fv_dlq.emplace_back("dlq_timestamp", std::to_string(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count()));

    // Use original message ID or a new one for DLQ key? Original key might not be unique over time.
    // Let's use original message ID as key for DLQ for traceability.
    try {
        m_dlq_producer->set(msg.id, fv_dlq);
    } catch (const std::exception& e) {
        SWSS_LOG_ERROR("AdvancedConsumerTable ('%s'): Exception while moving Message ID '%s' to DLQ: %s",
                       getTableName().c_str(), msg.id.c_str(), e.what());
        // TODO: What to do if DLQ write fails? Circuit breaker? Log and drop?
    }
}

// --- Selectable Overrides ---
int AdvancedConsumerTable::getFd() {
    return m_event_fd;
}

bool AdvancedConsumerTable::hasData() {
    // This should reflect if m_event_fd would block or not.
    // If m_priority_queue_out is not empty, eventfd should be signaled.
    // The base ConsumerTable::hasData() might also be relevant if we were using a combined FD approach.
    // For now, tied directly to m_priority_queue_out state.
    return !m_priority_queue_out.empty();
}

bool AdvancedConsumerTable::hasCachedData() {
    // For this implementation, cached data is what's in m_priority_queue_out.
    return !m_priority_queue_out.empty();
}

uint64_t AdvancedConsumerTable::readData() {
    // This method is called when the Select framework detects activity on getFd().
    // For eventfd, we read the eventfd counter to clear it.
    // The actual message data is retrieved via pops().
    updateAfterRead(); // Clear the eventfd
    // Return value could indicate rough number of items, or just 0/1.
    // Returning 0 as the data is not "read" in the traditional sense from eventfd.
    return 0;
}

void AdvancedConsumerTable::updateAfterRead() {
    uint64_t count;
    ssize_t ret = ::read(m_event_fd, &count, sizeof(uint64_t));
    if (ret == -1) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            // This is expected if eventfd was not actually signaled (e.g., spurious wakeup)
            // or if another thread/call already read it.
            SWSS_LOG_DEBUG("AdvancedConsumerTable ('%s'): eventfd_read returned EAGAIN/EWOULDBLOCK.", getTableName().c_str());
        } else {
            SWSS_LOG_ERROR("AdvancedConsumerTable ('%s'): eventfd_read failed: %s", getTableName().c_str(), strerror(errno));
            // Potentially throw or handle error more robustly.
        }
    } else if (ret != sizeof(uint64_t)) {
        SWSS_LOG_ERROR("AdvancedConsumerTable ('%s'): eventfd_read read incorrect number of bytes: %ld", getTableName().c_str(), ret);
        // Potentially throw or handle error.
    } else {
        SWSS_LOG_DEBUG("AdvancedConsumerTable ('%s'): eventfd_read successful, count was %lu.", getTableName().c_str(), count);
    }
}

bool AdvancedConsumerTable::initializedWithData() {
    // Try to populate the queue to see if data is available on initialization.
    processMessagesFromRedisToOutgoingQueue(0);
    return !m_priority_queue_out.empty();
}


// --- Deserialization and Serialization Helpers ---
bool AdvancedConsumerTable::deserializeMessage(const KeyOpFieldsValuesTuple& kco, common::Message& message) {
    // The key from Redis is the definitive original_key for this consumer instance.
    message.original_key = kco.first;
    // kco.second contains the operation, not used for deserializing message content here.

    if (kco.third.empty() || kco.third[0].first != common::Message::MSG_PAYLOAD_FIELD) {
        SWSS_LOG_WARN("AdvancedConsumerTable ('%s'): Expected field '%s' not found or KCO fields empty for key '%s'. Cannot deserialize full message.",
                      getTableName().c_str(), common::Message::MSG_PAYLOAD_FIELD, kco.first.c_str());
        // Attempt to provide a minimal message if possible, or indicate failure.
        // If we cannot get the full message, it's an error or unexpected format.
        // For strictness, let's return false.
        // Optionally, one could try to populate message.content with kco.third if a fallback is desired.
        return false;
    }

    const std::string& serialized_full_message = kco.third[0].second;

    try {
        Json json_obj = Json::parse(serialized_full_message);
        from_json(json_obj, message); // Uses the from_json function defined above in swss namespace

        // After from_json, message.original_key is populated from the *payload*.
        // We re-assert the key from Redis as the primary key for this consumed instance.
        message.original_key = kco.first;
        return true;
    } catch (const std::exception& e) {
        SWSS_LOG_ERROR("AdvancedConsumerTable ('%s'): Exception during full message deserialization for key '%s': %s. Payload: %s",
                       getTableName().c_str(), kco.first.c_str(), e.what(), serialized_full_message.c_str());
        return false;
    }
}

KeyOpFieldsValuesTuple AdvancedConsumerTable::serializeMessageToKCO(const common::Message& message) const {
    KeyOpFieldsValuesTuple kco;
    kco.first = message.original_key;
    // kco.second (operation) is typically not set by consumer side for placing back in deque,
    // but if it were to be *sent* via ProducerTable, it would be SET_COMMAND or DEL_COMMAND.
    // For now, let's leave it empty or use a generic marker if necessary.
    // kco.op = SET_COMMAND; // If needed

    // message.content is a JSON string representing the original std::vector<FieldValueTuple>
    std::vector<FieldValueTuple> original_values;
    if (!Json::deserialize(message.content, original_values)) {
        SWSS_LOG_ERROR("AdvancedConsumerTable ('%s'): Failed to deserialize message.content (original FVT list) for key '%s' during KCO serialization. Content: %s",
                       getTableName().c_str(), message.original_key.c_str(), message.content.c_str());
        // Depending on strictness, either return kco with empty original_values or throw.
        // For now, proceed with empty original_values if deserialization fails.
    }
    kco_result.third = original_values; // For std::pair, .second holds the vector<FV>
    return kco_result;
}

// --- pops Overrides ---
void AdvancedConsumerTable::pops(std::deque<KeyOpFieldsValuesTuple>& vkco, const std::string& prefix) {
    SWSS_LOG_DEBUG("AdvancedConsumerTable ('%s'): pops(vkco, prefix='%s') called.", getTableName().c_str(), prefix.c_str());
    if (!prefix.empty()) {
        SWSS_LOG_WARN("AdvancedConsumerTable ('%s'): The 'prefix' parameter in pops(vkco, prefix) is not fully honored by the current pre-fetching logic of processMessagesFromRedisToOutgoingQueue. Fetched messages are from the table's base, then filtered.", getTableName().c_str());
    }

    vkco.clear();
    // Ensure m_priority_queue_out is populated. The '0' means process all from current m_redis_buffer.
    processMessagesFromRedisToOutgoingQueue(0);

    size_t count = 0;
    // getPopBatchSize() is from ConsumerTableBase, defines max items to pop in one go.
    size_t batch_limit = static_cast<size_t>(getPopBatchSize());

    while (!m_priority_queue_out.empty() && count < batch_limit) {
        common::Message msg = m_priority_queue_out.pop(); // pop also removes from queue

        if (m_manual_ack_enabled) {
            m_pending_ack_messages[msg.id] = msg;
            SWSS_LOG_DEBUG("AdvancedConsumerTable ('%s'): Message ID '%s' (Key: '%s') moved to pending ACK for vkco.pops().",
                           getTableName().c_str(), msg.id.c_str(), msg.original_key.c_str());
        }
        // Auto-ack mode: Logged during processing or considered implicitly handled.

        vkco.push_back(serializeMessageToKCO(msg)); // This now returns std::pair<string, vector<FV>>
        count++;
    }
    SWSS_LOG_DEBUG("AdvancedConsumerTable ('%s'): pops(vkco) returning %zu messages.", getTableName().c_str(), vkco.size());
    // If m_priority_queue_out is now empty, the eventfd should reflect that (it's level-triggered,
    // but updateAfterRead would have cleared it if it was read by Select).
    // If it's not empty, eventfd should still be signaled (or will be re-signaled if new data comes).
}

void AdvancedConsumerTable::pops(std::deque<common::Message>& messages, size_t max_messages_to_pop) {
    SWSS_LOG_DEBUG("AdvancedConsumerTable ('%s'): pops(messages, max_messages=%zu) called.", getTableName().c_str(), max_messages_to_pop);
    messages.clear();

    // Ensure m_priority_queue_out is populated from any buffered Redis data.
    // The '0' means process all from current m_redis_buffer.
    processMessagesFromRedisToOutgoingQueue(0);

    // Determine the actual limit for this pop operation.
    // If max_messages_to_pop is 0, use popBatchSize from ConsumerTable as a default batch limit.
    // Otherwise, use the smaller of max_messages_to_pop and popBatchSize to respect both constraints.
    size_t batch_limit = static_cast<size_t>(getPopBatchSize()); // popBatchSize is from ConsumerTableBase
    size_t limit = (max_messages_to_pop == 0) ? batch_limit : std::min(max_messages_to_pop, batch_limit);


    while (!m_priority_queue_out.empty() && messages.size() < limit) {
        common::Message msg = m_priority_queue_out.pop(); // pop also removes from queue

        if (m_manual_ack_enabled) {
            m_pending_ack_messages[msg.id] = msg; // Store the exact message popped
             SWSS_LOG_DEBUG("AdvancedConsumerTable ('%s'): Message ID '%s' (Key: '%s') moved to pending ACK for messages.pops().",
                           getTableName().c_str(), msg.id.c_str(), msg.original_key.c_str());
        } else {
            // Auto-ack mode. For AT_MOST_ONCE, this is fine.
            // For AT_LEAST_ONCE/EXACTLY_ONCE, this would be where the message is implicitly confirmed.
            // Currently, no explicit action for auto-ack beyond not putting in m_pending_ack_messages.
            SWSS_LOG_DEBUG("AdvancedConsumerTable ('%s'): Message ID '%s' (Key: '%s') auto-acknowledged (or not requiring manual ack).",
                           getTableName().c_str(), msg.id.c_str(), msg.original_key.c_str());
        }
        messages.push_back(msg);
    }
    SWSS_LOG_DEBUG("AdvancedConsumerTable ('%s'): pops(messages) returning %zu messages.", getTableName().c_str(), messages.size());
    // If m_priority_queue_out is now empty, eventfd state is consistent.
    // If items remain, eventfd should still be signaled if it was read.
    // The next call to select() will determine if it's still readable.
}

// --- Main Processing Logic ---
void AdvancedConsumerTable::processMessagesFromRedisToOutgoingQueue(size_t max_to_process_from_redis_buffer) {
    // If m_redis_buffer is empty, try to fill it from Redis.
    // The `max_to_process_from_redis_buffer` parameter is currently unused here,
    // as ConsumerTable::pops typically fetches based on its own batch size.
    // This parameter might be more relevant if we had a larger internal buffer
    // that we are incrementally processing. For now, we process whatever is fetched.
    if (m_redis_buffer.empty()) {
        ConsumerTable::pops(m_redis_buffer, ""); // Using empty prefix
        SWSS_LOG_DEBUG("AdvancedConsumerTable ('%s'): Fetched %zu raw items from Redis into m_redis_buffer.",
                       getTableName().c_str(), m_redis_buffer.size());
    }

    if (m_redis_buffer.empty()) {
        return; // No data from Redis to process
    }

    bool new_messages_added_to_outgoing_queue = false;
    std::deque<KeyOpFieldsValuesTuple> unprocessed_raw_items; // Temp buffer for items that couldn't be processed in this pass

    for (const auto& kco : m_redis_buffer) {
        common::Message current_msg;
        if (deserializeMessage(kco, current_msg)) {
            if (applyFilters(current_msg)) {
                m_priority_queue_out.push(current_msg);
                new_messages_added_to_outgoing_queue = true;
                SWSS_LOG_INFO("AdvancedConsumerTable ('%s'): Message ID '%s' (Key: '%s') accepted and queued to m_priority_queue_out.",
                               getTableName().c_str(), current_msg.id.c_str(), current_msg.original_key.c_str());
            } else {
                SWSS_LOG_INFO("AdvancedConsumerTable ('%s'): Message ID '%s' (Key: '%s') was filtered out.",
                               getTableName().c_str(), current_msg.id.c_str(), current_msg.original_key.c_str());
                // TODO: Add configurable option to move filtered messages to DLQ.
                // For now, filtered messages are logged and discarded.
            }
        } else {
            SWSS_LOG_ERROR("AdvancedConsumerTable ('%s'): Failed to deserialize message for key '%s'. Moving to DLQ.",
                           getTableName().c_str(), kco.first.c_str());

            // Create a "raw" message for DLQ
            common::Message malformed_msg_for_dlq;
            malformed_msg_for_dlq.id = kco.first + "_" + std::to_string(std::chrono::system_clock::now().time_since_epoch().count()); // Create a unique ID
            malformed_msg_for_dlq.original_key = kco.first;
            malformed_msg_for_dlq.priority = 0; // Default priority
            malformed_msg_for_dlq.timestamp = std::chrono::system_clock::now();

            // Attempt to serialize the problematic kco.third to JSON for the content.
            // If kco.third itself is problematic, this might also fail or produce minimal output.
            try {
                malformed_msg_for_dlq.content = Json::serialize(kco.third);
            } catch (const std::exception& e) {
                SWSS_LOG_ERROR("AdvancedConsumerTable ('%s'): Could not serialize kco.third for DLQ message (key: %s): %s",
                    getTableName().c_str(), kco.first.c_str(), e.what());
                std::vector<FieldValueTuple> error_fv;
                error_fv.emplace_back("error", "failed to serialize original kco.third");
                if (!kco.third.empty()) {
                     error_fv.emplace_back("original_field_0_name", kco.third[0].first);
                     error_fv.emplace_back("original_field_0_value", kco.third[0].second); // May be truncated if very long
                }
                malformed_msg_for_dlq.content = Json::serialize(error_fv);
            }
            moveToDLQ(malformed_msg_for_dlq, "DeserializationError");
        }
    }

    m_redis_buffer.clear(); // All items from m_redis_buffer have been processed or moved to DLQ.

    if (new_messages_added_to_outgoing_queue && m_event_fd != -1) {
        uint64_t count = 1; // Signal that new data is available
        ssize_t s = ::write(m_event_fd, &count, sizeof(uint64_t));
        if (s != sizeof(uint64_t)) {
            SWSS_LOG_ERROR("AdvancedConsumerTable ('%s'): Failed to write to eventfd: %s",
                           getTableName().c_str(), strerror(errno));
            // Handle error, though eventfd write failures are rare with a valid fd.
        } else {
            SWSS_LOG_DEBUG("AdvancedConsumerTable ('%s'): Signaled eventfd due to new messages in m_priority_queue_out. Current size: %zu.",
                           getTableName().c_str(), m_priority_queue_out.size());
        }
    }
}

// --- Apply Filters (Stub/Basic Outline) ---

// Helper to reconstruct message from status hash (minimal for DLQ or timeout processing)
// Returns true if essential fields could be reconstructed, false otherwise.
// Note: message.content will typically NOT be available from the status hash.
bool AdvancedConsumerTable::reconstructMessageFromStatus(const std::string& message_id, common::Message& message) {
    if (!m_ack_db_connector) {
        SWSS_LOG_ERROR("AdvancedConsumerTable ('%s'): ACK DB connector not initialized, cannot reconstruct message %s from status.",
                       getTableName().c_str(), message_id.c_str());
        return false;
    }
    std::string status_key = std::string(common::Message::MSG_STATUS_PREFIX) + message_id;
    std::map<std::string, std::string> status_hash_map;

    try {
        m_ack_db_connector->getRedisContext()->hgetall(status_key, std::inserter(status_hash_map, status_hash_map.begin()));
    } catch (const std::exception& e) {
        SWSS_LOG_ERROR("AdvancedConsumerTable ('%s'): Exception reading status hash for message ID %s (key: %s): %s",
                       getTableName().c_str(), message_id.c_str(), status_key.c_str(), e.what());
        return false;
    }

    if (status_hash_map.empty()) {
        SWSS_LOG_WARN("AdvancedConsumerTable ('%s'): No status fields found for message ID %s in Redis (key: %s). Cannot reconstruct message.",
                      getTableName().c_str(), message_id.c_str(), status_key.c_str());
        return false;
    }

    message.id = message_id;
    message.original_key = status_hash_map[common::Message::MSG_STAT_FIELD_TABLE_NAME] + ":" + message_id; // Best guess for original key context
    if (status_hash_map.count("original_key")) { // If producer stored it
         message.original_key = status_hash_map["original_key"];
    }


    if (status_hash_map.count(common::Message::MSG_STAT_FIELD_TIMESTAMP)) {
        try {
            long long timestamp_ms = std::stoll(status_hash_map[common::Message::MSG_STAT_FIELD_TIMESTAMP]);
            message.timestamp = std::chrono::time_point<std::chrono::system_clock>(std::chrono::milliseconds(timestamp_ms));
        } catch (const std::exception& e) { /* log below */ }
    }
    if (status_hash_map.count(common::Message::MSG_STAT_FIELD_CORR_ID)) {
        message.correlation_id = status_hash_map[common::Message::MSG_STAT_FIELD_CORR_ID];
    }
    if (status_hash_map.count(common::Message::MSG_STAT_FIELD_DELIVERY_MODE)) {
        try {
            message.delivery_mode = static_cast<common::DeliveryMode>(std::stoi(status_hash_map[common::Message::MSG_STAT_FIELD_DELIVERY_MODE]));
        } catch (const std::exception &e) {message.delivery_mode = common::DeliveryMode::AT_LEAST_ONCE; /* fallback */}
    }
    if (status_hash_map.count("retry_count")) { // "retry_count" is not in MSG_STAT_FIELD constants yet
         try {
            message.retry_count = std::stoi(status_hash_map["retry_count"]);
        } catch (const std::exception &e) { /* log or ignore */ }
    }
    // message.content is not stored in the status hash, so it will be empty by default.
    // message.priority is also not typically stored in status hash, defaults to 0 or some value.
    message.priority = 0; // Default as it's not in status hash

    return true;
}


void AdvancedConsumerTable::processTimedOutMessages() {
    if (!m_ack_db_connector) {
        SWSS_LOG_ERROR("AdvancedConsumerTable ('%s'): ACK DB connector not initialized, cannot process timed out messages.", getTableName().c_str());
        return;
    }

    long long current_time_ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
    long long timeout_threshold_ms = current_time_ms - m_config.ack_timeout_ms;

    SWSS_LOG_DEBUG("AdvancedConsumerTable ('%s'): Processing timed out messages with threshold %lld ms (current: %lld, timeout: %d).",
                   getTableName().c_str(), timeout_threshold_ms, current_time_ms, m_config.ack_timeout_ms);

    std::vector<std::string> timed_out_message_ids;
    try {
        timed_out_message_ids = m_ack_db_connector->getRedisContext()->zrangebyscore(
            common::Message::PENDING_MESSAGES_BY_TIME_KEY,
            "0",
            std::to_string(static_cast<double>(timeout_threshold_ms))
        );
    } catch (const std::exception& e) {
        SWSS_LOG_ERROR("AdvancedConsumerTable ('%s'): Exception getting timed out messages from Redis: %s", getTableName().c_str(), e.what());
        return;
    }

    if (!timed_out_message_ids.empty()) {
        SWSS_LOG_INFO("AdvancedConsumerTable ('%s'): Found %zu potentially timed out message(s).", getTableName().c_str(), timed_out_message_ids.size());
    }


    for (const auto& msg_id : timed_out_message_ids) {
        common::Message message_details;
        std::string status_key = std::string(common::Message::MSG_STATUS_PREFIX) + msg_id;
        std::map<std::string, std::string> status_hash;

        try {
            m_ack_db_connector->getRedisContext()->hgetall(status_key, std::inserter(status_hash, status_hash.begin()));

            if (status_hash.empty() || !status_hash.count(common::Message::MSG_STAT_FIELD_STATUS)) {
                 SWSS_LOG_WARN("AdvancedConsumerTable ('%s'): Message ID %s found in timeout set but status key %s or status field missing/empty. Removing from timeout set.",
                               getTableName().c_str(), msg_id.c_str(), status_key.c_str());
                m_ack_db_connector->getRedisContext()->zrem(common::Message::PENDING_MESSAGES_BY_TIME_KEY, {msg_id});
                continue;
            }
        } catch (const std::exception& e) {
            SWSS_LOG_ERROR("AdvancedConsumerTable ('%s'): Exception reading status for timed out message ID %s: %s. Skipping.",
                           getTableName().c_str(), msg_id.c_str(), e.what());
            continue;
        }

        std::string current_status_str = status_hash[common::Message::MSG_STAT_FIELD_STATUS];

        // Reconstruct message for retry_count and other metadata. Content will be empty.
        // original_key in message_details will be from status hash if producer stored it there.
        if (!reconstructMessageFromStatus(msg_id, message_details)) {
             SWSS_LOG_ERROR("AdvancedConsumerTable ('%s'): Failed to reconstruct details for timed out message ID %s. Removing from timeout set.",
                           getTableName().c_str(), msg_id.c_str());
            m_ack_db_connector->getRedisContext()->zrem(common::Message::PENDING_MESSAGES_BY_TIME_KEY, {msg_id});
            continue;
        }


        if (current_status_str == "PENDING") {
            SWSS_LOG_WARN("AdvancedConsumerTable ('%s'): Message ID %s (Orig. Key: %s) timed out (status: PENDING). Current Retry count from status: %d.",
                          getTableName().c_str(), msg_id.c_str(), message_details.original_key.c_str(), message_details.retry_count);

            message_details.retry_count++; // Increment for this timeout event
            long long new_timestamp_ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();

            if (message_details.retry_count < m_config.max_retries) {
                std::vector<std::pair<std::string, std::string>> fields_to_update;
                fields_to_update.emplace_back(common::Message::MSG_STAT_FIELD_STATUS, "PENDING");
                fields_to_update.emplace_back(common::Message::MSG_STAT_FIELD_TIMESTAMP, std::to_string(new_timestamp_ms));
                fields_to_update.emplace_back("retry_count", std::to_string(message_details.retry_count));
                fields_to_update.emplace_back("last_nack_reason", "ProcessingTimeout");

                try {
                    m_ack_db_connector->getRedisContext()->hmset(status_key, fields_to_update.begin(), fields_to_update.end());
                    // Update score to new timestamp to give it another ack_timeout_ms period
                    m_ack_db_connector->getRedisContext()->zadd(common::Message::PENDING_MESSAGES_BY_TIME_KEY, static_cast<double>(new_timestamp_ms), msg_id);
                    SWSS_LOG_INFO("AdvancedConsumerTable ('%s'): Message ID %s marked for re-delivery attempt (retry %d/%d) due to timeout. Score updated in PENDING_MSG_TS.",
                                  getTableName().c_str(), msg_id.c_str(), message_details.retry_count, m_config.max_retries);
                } catch (const std::exception& e) {
                    SWSS_LOG_ERROR("AdvancedConsumerTable ('%s'): Failed to update message %s for timeout re-delivery: %s", getTableName().c_str(), msg_id.c_str(), e.what());
                }
            } else {
                SWSS_LOG_ERROR("AdvancedConsumerTable ('%s'): Message ID %s (Orig. Key: %s) timed out and exceeded max retries (%d). Moving to DLQ.",
                               getTableName().c_str(), msg_id.c_str(), message_details.original_key.c_str(), m_config.max_retries);
                try {
                    std::vector<std::pair<std::string, std::string>> fields_to_update;
                    fields_to_update.emplace_back(common::Message::MSG_STAT_FIELD_STATUS, "NACKED");
                    fields_to_update.emplace_back(common::Message::MSG_STAT_FIELD_TIMESTAMP, std::to_string(new_timestamp_ms));
                    fields_to_update.emplace_back("nack_reason", "ProcessingTimeout_MaxRetriesExceeded");
                    fields_to_update.emplace_back("retry_count", std::to_string(message_details.retry_count));
                    m_ack_db_connector->getRedisContext()->hmset(status_key, fields_to_update.begin(), fields_to_update.end());

                    m_ack_db_connector->getRedisContext()->zrem(common::Message::PENDING_MESSAGES_BY_TIME_KEY, {msg_id});
                    if (!message_details.correlation_id.empty()) {
                        std::string corr_key = std::string(common::Message::CORRELATION_IDS_PREFIX) + message_details.correlation_id;
                        m_ack_db_connector->getRedisContext()->srem(corr_key, { msg_id });
                    }

                    // message_details.content is empty from reconstructMessageFromStatus.
                    // moveToDLQ for timed-out messages will have minimal content.
                    moveToDLQ(message_details, "ProcessingTimeout_MaxRetriesExceeded");
                } catch (const std::exception& e) {
                     SWSS_LOG_ERROR("AdvancedConsumerTable ('%s'): Failed to move message %s to DLQ after timeout max retries: %s", getTableName().c_str(), msg_id.c_str(), e.what());
                }
            }
        } else {
            SWSS_LOG_INFO("AdvancedConsumerTable ('%s'): Message ID %s found in timeout set but status is '%s' (not PENDING). Removing from timeout set.",
                          getTableName().c_str(), msg_id.c_str(), current_status_str.c_str());
            try {
                m_ack_db_connector->getRedisContext()->zrem(common::Message::PENDING_MESSAGES_BY_TIME_KEY, {msg_id});
            } catch (const std::exception& e) {
                SWSS_LOG_ERROR("AdvancedConsumerTable ('%s'): Failed to ZREM message %s from timeout set: %s", getTableName().c_str(), msg_id.c_str(), e.what());
            }
        }
    }
}


// Placeholder for glob_match function
// In a real scenario, this would use a library like fnmatch or a custom implementation.
bool glob_match(const std::string& pattern, const std::string& text) {
// In a real scenario, this would use a library like fnmatch or a custom implementation.
bool glob_match(const std::string& pattern, const std::string& text) {
    if (pattern.empty()) return text.empty();
    if (pattern == "*") return true; // Simplistic wildcard support
    // TODO: Implement or integrate a proper glob matching function.
    // For now, treat as simple string comparison if not "*"
    if (pattern.find('*') == std::string::npos && pattern.find('?') == std::string::npos) {
        return pattern == text;
    }
    SWSS_LOG_WARN("glob_match: Pattern '%s' with wildcards other than a single '*' is not fully supported in this stub.", pattern.c_str());
    // Fallback for complex patterns not handled by this stub: treat as non-match or simple checks.
    // This basic stub will mostly work for exact matches or a single '*' wildcard.
    if (pattern[0] == '*' && pattern.length() > 1) {
        return text.length() >= pattern.length() -1 && text.rfind(pattern.substr(1)) == text.length() - (pattern.length() -1);
    }
    if (pattern.back() == '*' && pattern.length() > 1) {
        return text.rfind(pattern.substr(0,pattern.length()-1),0) == 0;
    }

    return pattern == text; // Default to exact match for unhandled patterns
}

bool AdvancedConsumerTable::applyFilters(const common::Message& msg) {
    SWSS_LOG_DEBUG("AdvancedConsumerTable ('%s'): Applying filters to Message ID '%s', Key '%s'",
                   getTableName().c_str(), msg.id.c_str(), msg.original_key.c_str());

    for (const auto& filter_conf_base : m_filters) {
        if (!filter_conf_base) continue;

        SWSS_LOG_DEBUG("AdvancedConsumerTable ('%s'): Evaluating filter ID '%s'", getTableName().c_str(), filter_conf_base->filter_id.c_str());

        if (auto* key_glob_filter = dynamic_cast<common::KeyGlobFilterConfig*>(filter_conf_base.get())) {
            // Dependency: glob_match function (stubbed above for now)
            if (!glob_match(key_glob_filter->glob_pattern, msg.original_key)) {
                SWSS_LOG_DEBUG("AdvancedConsumerTable ('%s'): Message ID '%s' (Key: '%s') FAILED KeyGlobFilter (Pattern: '%s')",
                               getTableName().c_str(), msg.id.c_str(), msg.original_key.c_str(), key_glob_filter->glob_pattern.c_str());
                return false;
            }
            SWSS_LOG_DEBUG("AdvancedConsumerTable ('%s'): Message ID '%s' (Key: '%s') PASSED KeyGlobFilter (Pattern: '%s')",
                               getTableName().c_str(), msg.id.c_str(), msg.original_key.c_str(), key_glob_filter->glob_pattern.c_str());

        } else if (auto* json_path_filter = dynamic_cast<common::JsonPathFilterConfig*>(filter_conf_base.get())) {
            // Dependency: JSONPath evaluation library (complex to stub simply)
            // msg.content is a JSON string of the original FieldValueTuple vector.
            // For a simple stub, deserialize msg.content and check a direct field if json_path_expression is simple.
            SWSS_LOG_DEBUG("AdvancedConsumerTable ('%s'): Applying JsonPathFilter (ID: %s, Path: '%s', Expected: '%s') to Message ID '%s' (Content: '%s')",
                getTableName().c_str(), json_path_filter->filter_id.c_str(), json_path_filter->json_path_expression.c_str(),
                json_path_filter->expected_value.c_str(), msg.id.c_str(), msg.content.c_str());

            // STUB for JSONPath: This is a very simplified stub.
            // It assumes msg.content is a JSON representation of std::vector<FieldValueTuple>
            // and json_path_expression is a direct key in that list of fields.
            std::vector<FieldValueTuple> fields;
            if (!Json::deserialize(msg.content, fields)) {
                SWSS_LOG_ERROR("AdvancedConsumerTable ('%s'): Failed to deserialize msg.content for JsonPathFilter on Message ID '%s'. Content: %s",
                               getTableName().c_str(), msg.id.c_str(), msg.content.c_str());
                return false; // Cannot apply filter if content is not as expected
            }

            bool found_match = false;
            for (const auto& field : fields) {
                if (field.first == json_path_filter->json_path_expression) {
                    if (field.second == json_path_filter->expected_value) {
                        found_match = true;
                        break;
                    }
                }
            }
            if (!found_match) {
                 SWSS_LOG_DEBUG("AdvancedConsumerTable ('%s'): Message ID '%s' FAILED JsonPathFilter (Path: '%s', Expected: '%s')",
                               getTableName().c_str(), msg.id.c_str(), json_path_filter->json_path_expression.c_str(), json_path_filter->expected_value.c_str());
                return false;
            }
            SWSS_LOG_DEBUG("AdvancedConsumerTable ('%s'): Message ID '%s' PASSED JsonPathFilter (Path: '%s', Expected: '%s')",
                               getTableName().c_str(), msg.id.c_str(), json_path_filter->json_path_expression.c_str(), json_path_filter->expected_value.c_str());
        }
        // Note: CustomPredicateFilterConfig is not handled here as per decision to keep m_custom_message_filter separate.
        // If it were part of m_filters, its handling would be here.
    }

    if (m_custom_message_filter) {
        SWSS_LOG_DEBUG("AdvancedConsumerTable ('%s'): Applying custom message filter to Message ID '%s'", getTableName().c_str(), msg.id.c_str());
        if (!m_custom_message_filter(msg)) {
            SWSS_LOG_DEBUG("AdvancedConsumerTable ('%s'): Message ID '%s' FAILED custom message filter.", getTableName().c_str(), msg.id.c_str());
            return false; // Custom filter failed
        }
        SWSS_LOG_DEBUG("AdvancedConsumerTable ('%s'): Message ID '%s' PASSED custom message filter.", getTableName().c_str(), msg.id.c_str());
    }

    SWSS_LOG_DEBUG("AdvancedConsumerTable ('%s'): Message ID '%s' (Key: '%s') PASSED ALL filters.",
                   getTableName().c_str(), msg.id.c_str(), msg.original_key.c_str());
    return true; // All filters passed or no filters are configured
}

} // namespace swss
