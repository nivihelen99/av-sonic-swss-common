#include "common/advancedconsumertable.h"
#include "common/json.h"
#include "common/logger.h"
#include "common/table.h"     // For FieldValueTuple
#include "common/redisapi.h"  // For various Redis constants
#include "common/producertable.h" // For m_dlq_producer

#include <vector>
#include <utility> // For std::pair
#include <chrono>  // For std::chrono types
#include <sys/eventfd.h> // For eventfd
#include <unistd.h> // For close()
#include <cerrno>   // For errno
#include <cstring>  // For strerror
#include <algorithm> // for std::min


// Helper to deserialize swss::Json to common::Message
namespace swss {
    void from_json(const Json& j, common::Message& m) {
        j.at("id").get_to(m.id);
        j.at("priority").get_to(m.priority);
        j.at("original_key").get_to(m.original_key);
        if (j.count("original_table_name")) j.at("original_table_name").get_to(m.original_table_name); else m.original_table_name = "";
        j.at("content").get_to(m.content);
        long long timestamp_ms = 0;
        if (j.count("timestamp_ms")) j.at("timestamp_ms").get_to(timestamp_ms);
        m.timestamp = std::chrono::time_point<std::chrono::system_clock>(std::chrono::milliseconds(timestamp_ms));
        if (j.count("retry_count")) j.at("retry_count").get_to(m.retry_count); else m.retry_count = 0;
        if (j.count("correlation_id")) j.at("correlation_id").get_to(m.correlation_id); else m.correlation_id = "";
        int delivery_mode_int = 0;
        if (j.count("delivery_mode")) j.at("delivery_mode").get_to(delivery_mode_int);
        m.delivery_mode = static_cast<common::DeliveryMode>(delivery_mode_int);

        // DLQ fields
        if (j.count("dlq_reason")) j.at("dlq_reason").get_to(m.dlq_reason); else m.dlq_reason = "";
        if (j.count("dlq_timestamp_ms")) j.at("dlq_timestamp_ms").get_to(m.dlq_timestamp_ms); else m.dlq_timestamp_ms = 0;
        if (j.count("last_nack_error_message")) j.at("last_nack_error_message").get_to(m.last_nack_error_message); else m.last_nack_error_message = "";
    }
} // namespace swss


namespace swss {

// Constructor
AdvancedConsumerTable::AdvancedConsumerTable(DBConnector* db, const std::string& tableName, int popBatchSize)
    : ConsumerTable(db, tableName, popBatchSize),
      m_db(db),
      m_ack_db_connector(db),
      m_manual_ack_enabled(false),
      m_dlq_table_name(tableName + "_DLQ"),
      m_event_fd(-1) {
    // m_config is default-initialized. Values like max_retries come from PubSubConfig defaults.
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
    if (message_id.empty()) {
         SWSS_LOG_ERROR("AdvancedConsumerTable ('%s'): ack() called with empty message_id.", getTableName().c_str());
        return;
    }
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
                m_ack_db_connector->getRedisContext()->expire(status_key, m_config.dead_letter_retention_hours * 3600);

                SWSS_LOG_INFO("AdvancedConsumerTable ('%s'): Message ID '%s' status updated to ACKED in Redis.",
                              getTableName().c_str(), acked_message.id.c_str());

                m_ack_db_connector->getRedisContext()->zrem(common::Message::PENDING_MESSAGES_BY_TIME_KEY, { acked_message.id });

                if (!acked_message.correlation_id.empty()) {
                    std::string corr_key = std::string(common::Message::CORRELATION_IDS_PREFIX) + acked_message.correlation_id;
                    m_ack_db_connector->getRedisContext()->srem(corr_key, { acked_message.id });
                }
            } else {
                SWSS_LOG_WARN("AdvancedConsumerTable ('%s'): Status key for Message ID '%s' not found in Redis for ACK. Possibly expired or already processed.",
                              getTableName().c_str(), acked_message.id.c_str());
                m_ack_db_connector->getRedisContext()->zrem(common::Message::PENDING_MESSAGES_BY_TIME_KEY, { acked_message.id });
                 if (!acked_message.correlation_id.empty()) {
                    std::string corr_key = std::string(common::Message::CORRELATION_IDS_PREFIX) + acked_message.correlation_id;
                    m_ack_db_connector->getRedisContext()->srem(corr_key, { acked_message.id });
                }
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
    nacked_message.last_nack_error_message = error_message;

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
                    m_ack_db_connector->getRedisContext()->expire(status_key, m_config.ack_timeout_ms / 1000 + 60);
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
                     moveToDLQ(nacked_message, "NACKed_StatusKeyMissing_CannotRequeue"); // Pass by ref
                     m_ack_db_connector->getRedisContext()->zrem(common::Message::PENDING_MESSAGES_BY_TIME_KEY, { nacked_message.id });
                 }
            } catch (const std::exception& e) {
                SWSS_LOG_ERROR("AdvancedConsumerTable ('%s'): Exception updating status to PENDING for Message ID '%s' for requeue: %s. Not removing from local pending map.",
                               getTableName().c_str(), nacked_message.id.c_str(), e.what());
                remove_from_pending_map = false;
            }
        } else {
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
                          getTableName().c_str(), nacked_message.id.c_str(), nacked_message.retry_count, nacked_message.last_nack_error_message.c_str());
            moveToDLQ(nacked_message, "NACKed_FinalNoRequeue"); // Pass by ref
        }
    } else {
        SWSS_LOG_INFO("AdvancedConsumerTable ('%s'): Message ID '%s' (mode: AT_MOST_ONCE) NACKed by client. Error: '%s'. Moving to DLQ.",
                      getTableName().c_str(), message_id.c_str(), nacked_message.last_nack_error_message.c_str());
        moveToDLQ(nacked_message, "NACKed_AtMostOnce"); // Pass by ref
    }

    if (remove_from_pending_map) {
        m_pending_ack_messages.erase(it);
    }
}

// --- DLQ Access Methods ---
std::unique_ptr<ConsumerTable> AdvancedConsumerTable::getDeadLetterQueueReader() {
    SWSS_LOG_INFO("AdvancedConsumerTable ('%s'): Creating DLQ reader for table '%s'.",
                  getTableName().c_str(), m_dlq_table_name.c_str());
    return std::make_unique<ConsumerTable>(m_db, m_dlq_table_name, getPopBatchSize());
}

void AdvancedConsumerTable::ensureDlqProducer() {
    if (!m_dlq_producer && m_db) {
        SWSS_LOG_INFO("AdvancedConsumerTable ('%s'): Initializing DLQ producer for table '%s'.",
                      getTableName().c_str(), m_dlq_table_name.c_str());
        m_dlq_producer = std::make_unique<ProducerTable>(m_db, m_dlq_table_name);
    } else if (!m_db) {
         SWSS_LOG_ERROR("AdvancedConsumerTable ('%s'): Cannot initialize DLQ producer as DBConnector is null.", getTableName().c_str());
    }
}

void AdvancedConsumerTable::moveToDLQ(common::Message& msg, const std::string& reason) {
    ensureDlqProducer();
    if (!m_dlq_producer) {
        SWSS_LOG_ERROR("AdvancedConsumerTable ('%s'): DLQ producer not initialized. Cannot move Message ID '%s' to DLQ.",
                       getTableName().c_str(), msg.id.c_str());
        return;
    }

    msg.dlq_reason = reason;
    msg.dlq_timestamp_ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
    // msg.last_nack_error_message is expected to be set by nack() before calling this.

    SWSS_LOG_WARN("AdvancedConsumerTable ('%s'): Moving Message ID '%s' (Original Key: '%s', Original Table: '%s') to DLQ '%s'. Reason: %s, Last NACK Error: '%s'",
                  getTableName().c_str(), msg.id.c_str(), msg.original_key.c_str(), msg.original_table_name.c_str(), m_dlq_table_name.c_str(), msg.dlq_reason.c_str(), msg.last_nack_error_message.c_str());

    std::string serialized_dlq_message_string;
    try {
        Json json_dlq_message;
        to_json(json_dlq_message, msg);
        serialized_dlq_message_string = json_dlq_message.dump();
    } catch (const std::exception& e) {
        SWSS_LOG_ERROR("AdvancedConsumerTable ('%s'): Failed to serialize message ID '%s' for DLQ: %s. Storing minimal info.",
                       getTableName().c_str(), msg.id.c_str(), e.what());
        std::vector<FieldValueTuple> fallback_fv;
        fallback_fv.emplace_back("original_id", msg.id);
        fallback_fv.emplace_back("original_key", msg.original_key);
        fallback_fv.emplace_back("dlq_reason", reason);
        fallback_fv.emplace_back("dlq_serialization_error", e.what());
        fallback_fv.emplace_back("dlq_timestamp_ms", std::to_string(msg.dlq_timestamp_ms));
        if(!msg.last_nack_error_message.empty()) fallback_fv.emplace_back("last_nack_error_message", msg.last_nack_error_message);
        try {
            m_dlq_producer->set(msg.id, fallback_fv);
        } catch (const std::exception& e_set) {
            SWSS_LOG_ERROR("AdvancedConsumerTable ('%s'): Exception storing fallback DLQ info for Message ID '%s': %s",
                           getTableName().c_str(), msg.id.c_str(), e_set.what());
        }
        return;
    }

    std::vector<FieldValueTuple> dlq_payload_values;
    dlq_payload_values.emplace_back(common::Message::MSG_PAYLOAD_FIELD, serialized_dlq_message_string);

    try {
        m_dlq_producer->set(msg.id, dlq_payload_values);
    } catch (const std::exception& e) {
        SWSS_LOG_ERROR("AdvancedConsumerTable ('%s'): Exception while moving Message ID '%s' to DLQ: %s",
                       getTableName().c_str(), msg.id.c_str(), e.what());
    }
}

// --- Selectable Overrides ---
int AdvancedConsumerTable::getFd() {
    return m_event_fd;
}

bool AdvancedConsumerTable::hasData() {
    return !m_priority_queue_out.empty();
}

bool AdvancedConsumerTable::hasCachedData() {
    return !m_priority_queue_out.empty();
}

uint64_t AdvancedConsumerTable::readData() {
    updateAfterRead();
    return 0;
}

void AdvancedConsumerTable::updateAfterRead() {
    uint64_t count;
    ssize_t ret = ::read(m_event_fd, &count, sizeof(uint64_t));
    if (ret == -1) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            SWSS_LOG_DEBUG("AdvancedConsumerTable ('%s'): eventfd_read returned EAGAIN/EWOULDBLOCK.", getTableName().c_str());
        } else {
            SWSS_LOG_ERROR("AdvancedConsumerTable ('%s'): eventfd_read failed: %s", getTableName().c_str(), strerror(errno));
        }
    } else if (ret != sizeof(uint64_t)) {
        SWSS_LOG_ERROR("AdvancedConsumerTable ('%s'): eventfd_read read incorrect number of bytes: %ld", getTableName().c_str(), ret);
    } else {
        SWSS_LOG_DEBUG("AdvancedConsumerTable ('%s'): eventfd_read successful, count was %lu.", getTableName().c_str(), count);
    }
}

bool AdvancedConsumerTable::initializedWithData() {
    processMessagesFromRedisToOutgoingQueue(0);
    return !m_priority_queue_out.empty();
}


// --- Deserialization and Serialization Helpers ---
bool AdvancedConsumerTable::deserializeMessage(const KeyOpFieldsValuesTuple& kco, common::Message& message) {
    message.original_key = kco.first;

    if (kco.third.empty() || kco.third[0].first != common::Message::MSG_PAYLOAD_FIELD) {
        SWSS_LOG_WARN("AdvancedConsumerTable ('%s'): Expected field '%s' not found or KCO fields empty for key '%s'. Cannot deserialize full message.",
                      getTableName().c_str(), common::Message::MSG_PAYLOAD_FIELD, kco.first.c_str());
        return false;
    }
    const std::string& serialized_full_message = kco.third[0].second;
    try {
        Json json_obj = Json::parse(serialized_full_message);
        from_json(json_obj, message);
        message.original_key = kco.first;
        return true;
    } catch (const std::exception& e) {
        SWSS_LOG_ERROR("AdvancedConsumerTable ('%s'): Exception during full message deserialization for key '%s': %s. Payload: %s",
                       getTableName().c_str(), kco.first.c_str(), e.what(), serialized_full_message.c_str());
        return false;
    }
}

KeyOpFieldsValuesTuple AdvancedConsumerTable::serializeMessageToKCO(const common::Message& message) const {
    std::pair<std::string, std::vector<FieldValueTuple>> kfv_pair;
    kfv_pair.first = message.original_key;
    std::vector<FieldValueTuple> original_values;
    if (!Json::deserialize(message.content, original_values)) {
        SWSS_LOG_ERROR("AdvancedConsumerTable ('%s'): Failed to deserialize message.content (original FVT list) for key '%s' during KCO serialization. Content: %s",
                       getTableName().c_str(), message.original_key.c_str(), message.content.c_str());
    }
    kfv_pair.second = original_values;
    return kfv_pair;
}

// --- pops Overrides ---
void AdvancedConsumerTable::pops(std::deque<KeyOpFieldsValuesTuple>& vkco, const std::string& prefix) {
    if (!prefix.empty()) {
        SWSS_LOG_WARN("AdvancedConsumerTable ('%s'): The 'prefix' parameter in pops(vkco, prefix) is not fully honored by the current pre-fetching logic of processMessagesFromRedisToOutgoingQueue. Fetched messages are from the table's base, then filtered.", getTableName().c_str());
    }
    vkco.clear();
    processMessagesFromRedisToOutgoingQueue(0);
    size_t batch_limit = static_cast<size_t>(getPopBatchSize());
    while (!m_priority_queue_out.empty() && vkco.size() < batch_limit) {
        common::Message msg = m_priority_queue_out.pop();
        if (m_manual_ack_enabled) {
            m_pending_ack_messages[msg.id] = msg;
            SWSS_LOG_DEBUG("AdvancedConsumerTable ('%s'): Message ID '%s' (Key: '%s') moved to pending ACK for vkco.pops().",
                           getTableName().c_str(), msg.id.c_str(), msg.original_key.c_str());
        }
        vkco.push_back(serializeMessageToKCO(msg));
    }
    SWSS_LOG_DEBUG("AdvancedConsumerTable ('%s'): pops(vkco) returning %zu messages.", getTableName().c_str(), vkco.size());
}

void AdvancedConsumerTable::pops(std::deque<common::Message>& messages, size_t max_messages_to_pop) {
    messages.clear();
    processMessagesFromRedisToOutgoingQueue(0);
    size_t batch_limit = static_cast<size_t>(getPopBatchSize());
    size_t limit = (max_messages_to_pop == 0) ? batch_limit : std::min(max_messages_to_pop, batch_limit);
    while (!m_priority_queue_out.empty() && messages.size() < limit) {
        common::Message msg = m_priority_queue_out.pop();
        if (m_manual_ack_enabled) {
            m_pending_ack_messages[msg.id] = msg;
             SWSS_LOG_DEBUG("AdvancedConsumerTable ('%s'): Message ID '%s' (Key: '%s') moved to pending ACK for messages.pops().",
                           getTableName().c_str(), msg.id.c_str(), msg.original_key.c_str());
        } else {
            SWSS_LOG_DEBUG("AdvancedConsumerTable ('%s'): Message ID '%s' (Key: '%s') auto-acknowledged (or not requiring manual ack).",
                           getTableName().c_str(), msg.id.c_str(), msg.original_key.c_str());
        }
        messages.push_back(msg);
    }
    SWSS_LOG_DEBUG("AdvancedConsumerTable ('%s'): pops(messages) returning %zu messages.", getTableName().c_str(), messages.size());
}

// --- Main Processing Logic ---
void AdvancedConsumerTable::processMessagesFromRedisToOutgoingQueue(size_t max_to_process_from_redis_buffer) {
    if (m_redis_buffer.empty()) {
        ConsumerTable::pops(m_redis_buffer, "");
        SWSS_LOG_DEBUG("AdvancedConsumerTable ('%s'): Fetched %zu raw items from Redis into m_redis_buffer.",
                       getTableName().c_str(), m_redis_buffer.size());
    }
    if (m_redis_buffer.empty()) {
        return;
    }
    bool new_messages_added_to_outgoing_queue = false;
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
                 // moveToDLQ(current_msg, "FilteredOut"); // Optional: move filtered messages to DLQ
            }
        } else {
            SWSS_LOG_ERROR("AdvancedConsumerTable ('%s'): Failed to deserialize message for key '%s'. Moving to DLQ.",
                           getTableName().c_str(), kco.first.c_str());
            common::Message malformed_msg_for_dlq;
            malformed_msg_for_dlq.id = kco.first + "_" + std::to_string(std::chrono::system_clock::now().time_since_epoch().count());
            malformed_msg_for_dlq.original_key = kco.first;
            malformed_msg_for_dlq.original_table_name = getTableName();
            malformed_msg_for_dlq.priority = 0;
            malformed_msg_for_dlq.timestamp = std::chrono::system_clock::now();
            try {
                malformed_msg_for_dlq.content = Json::serialize(kco.third);
            } catch (const std::exception& e_ser) {
                SWSS_LOG_ERROR("AdvancedConsumerTable ('%s'): Could not serialize kco.third for DLQ message (key: %s): %s",
                    getTableName().c_str(), kco.first.c_str(), e_ser.what());
                malformed_msg_for_dlq.content = "{\"error\":\"failed to serialize original kco.third\"}";
            }
            moveToDLQ(malformed_msg_for_dlq, "DeserializationError");
        }
    }

    m_redis_buffer.clear();


    if (new_messages_added_to_outgoing_queue && m_event_fd != -1) {
        uint64_t count = 1;
        ssize_t s = ::write(m_event_fd, &count, sizeof(uint64_t));
        if (s != sizeof(uint64_t)) {
            SWSS_LOG_ERROR("AdvancedConsumerTable ('%s'): Failed to write to eventfd: %s",
                           getTableName().c_str(), strerror(errno));
        } else {
            SWSS_LOG_DEBUG("AdvancedConsumerTable ('%s'): Signaled eventfd due to new messages in m_priority_queue_out. Current size: %zu.",
                           getTableName().c_str(), m_priority_queue_out.size());
        }
    }
}

// --- Apply Filters (Stub/Basic Outline) ---

// Helper to reconstruct message from status hash (minimal for DLQ or timeout processing)
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
        SWSS_LOG_DEBUG("AdvancedConsumerTable ('%s'): No status fields found for message ID %s in Redis (key: %s). Cannot reconstruct. Might be processed.",
                      getTableName().c_str(), message_id.c_str(), status_key.c_str());
        return false;
    }
    message.id = message_id;
    if (status_hash_map.count(common::Message::MSG_STAT_FIELD_TABLE_NAME)) {
         message.original_table_name = status_hash_map[common::Message::MSG_STAT_FIELD_TABLE_NAME];
         message.original_key = message.original_table_name + ":" + message_id; // Default if specific original_key not in status
    }
    if (status_hash_map.count("original_key")) {
         message.original_key = status_hash_map["original_key"];
    }
    if (status_hash_map.count(common::Message::MSG_STAT_FIELD_TIMESTAMP)) {
        try {
            long long ts_ms = std::stoll(status_hash_map[common::Message::MSG_STAT_FIELD_TIMESTAMP]);
            message.timestamp = std::chrono::time_point<std::chrono::system_clock>(std::chrono::milliseconds(ts_ms));
        } catch (const std::exception& e) {
            SWSS_LOG_ERROR("AdvancedConsumerTable ('%s'): Failed to parse timestamp '%s' for message %s: %s", getTableName().c_str(), status_hash_map[common::Message::MSG_STAT_FIELD_TIMESTAMP].c_str(), message_id.c_str(), e.what());
        }
    }
    if (status_hash_map.count(common::Message::MSG_STAT_FIELD_CORR_ID)) {
        message.correlation_id = status_hash_map[common::Message::MSG_STAT_FIELD_CORR_ID];
    }
    if (status_hash_map.count(common::Message::MSG_STAT_FIELD_DELIVERY_MODE)) {
        try {
            message.delivery_mode = static_cast<common::DeliveryMode>(std::stoi(status_hash_map[common::Message::MSG_STAT_FIELD_DELIVERY_MODE]));
        } catch (const std::exception &e) {
            SWSS_LOG_ERROR("AdvancedConsumerTable ('%s'): Failed to parse delivery_mode '%s' for message %s: %s", getTableName().c_str(), status_hash_map[common::Message::MSG_STAT_FIELD_DELIVERY_MODE].c_str(), message_id.c_str(), e.what());
            message.delivery_mode = common::DeliveryMode::AT_LEAST_ONCE;
        }
    }
    if (status_hash_map.count("retry_count")) {
         try {
            message.retry_count = std::stoi(status_hash_map["retry_count"]);
        } catch (const std::exception &e) {
            SWSS_LOG_ERROR("AdvancedConsumerTable ('%s'): Failed to parse retry_count '%s' for message %s: %s", getTableName().c_str(), status_hash_map["retry_count"].c_str(), message_id.c_str(), e.what());
        }
    }
    message.content = "";
    message.priority = 0;
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
            common::Message::PENDING_MESSAGES_BY_TIME_KEY, "0", std::to_string(static_cast<double>(timeout_threshold_ms)));
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
        if (!reconstructMessageFromStatus(msg_id, message_details)) {
             SWSS_LOG_ERROR("AdvancedConsumerTable ('%s'): Failed to reconstruct details for timed out message ID %s. Removing from timeout set.",
                           getTableName().c_str(), msg_id.c_str());
            m_ack_db_connector->getRedisContext()->zrem(common::Message::PENDING_MESSAGES_BY_TIME_KEY, {msg_id});
            continue;
        }
        if (current_status_str == "PENDING") {
            SWSS_LOG_WARN("AdvancedConsumerTable ('%s'): Message ID %s (Orig. Table: %s, Orig. Key from status: %s) timed out (status: PENDING). Current Retry count from status: %d.",
                          getTableName().c_str(), msg_id.c_str(), message_details.original_table_name.c_str(), message_details.original_key.c_str(), message_details.retry_count);
            message_details.retry_count++;
            long long new_timestamp_ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
            message_details.last_nack_error_message = "ProcessingTimeout";
            if (message_details.retry_count < m_config.max_retries) {
                std::vector<std::pair<std::string, std::string>> fields_to_update;
                fields_to_update.emplace_back(common::Message::MSG_STAT_FIELD_STATUS, "PENDING");
                fields_to_update.emplace_back(common::Message::MSG_STAT_FIELD_TIMESTAMP, std::to_string(new_timestamp_ms));
                fields_to_update.emplace_back("retry_count", std::to_string(message_details.retry_count));
                fields_to_update.emplace_back("last_nack_reason", message_details.last_nack_error_message);
                try {
                    m_ack_db_connector->getRedisContext()->hmset(status_key, fields_to_update.begin(), fields_to_update.end());
                    m_ack_db_connector->getRedisContext()->zadd(common::Message::PENDING_MESSAGES_BY_TIME_KEY, static_cast<double>(new_timestamp_ms), msg_id);
                    SWSS_LOG_INFO("AdvancedConsumerTable ('%s'): Message ID %s marked for re-delivery attempt (retry %d/%d) due to timeout. Score updated in PENDING_MSG_TS.",
                                  getTableName().c_str(), msg_id.c_str(), message_details.retry_count, m_config.max_retries);
                } catch (const std::exception& e) {
                    SWSS_LOG_ERROR("AdvancedConsumerTable ('%s'): Failed to update message %s for timeout re-delivery: %s", getTableName().c_str(), msg_id.c_str(), e.what());
                }
            } else {
                SWSS_LOG_ERROR("AdvancedConsumerTable ('%s'): Message ID %s (Orig. Table: %s, Orig. Key from status: %s) timed out and exceeded max retries (%d). Moving to DLQ.",
                               getTableName().c_str(), msg_id.c_str(), message_details.original_table_name.c_str(), message_details.original_key.c_str(), m_config.max_retries);
                try {
                    std::vector<std::pair<std::string, std::string>> fields_to_update;
                    fields_to_update.emplace_back(common::Message::MSG_STAT_FIELD_STATUS, "NACKED");
                    fields_to_update.emplace_back(common::Message::MSG_STAT_FIELD_TIMESTAMP, std::to_string(new_timestamp_ms));
                    fields_to_update.emplace_back("nack_reason", message_details.last_nack_error_message + "_MaxRetriesExceeded");
                    fields_to_update.emplace_back("retry_count", std::to_string(message_details.retry_count));
                    m_ack_db_connector->getRedisContext()->hmset(status_key, fields_to_update.begin(), fields_to_update.end());
                    m_ack_db_connector->getRedisContext()->zrem(common::Message::PENDING_MESSAGES_BY_TIME_KEY, {msg_id});
                    if (!message_details.correlation_id.empty()) {
                        std::string corr_key = std::string(common::Message::CORRELATION_IDS_PREFIX) + message_details.correlation_id;
                        m_ack_db_connector->getRedisContext()->srem(corr_key, { msg_id });
                    }
                    moveToDLQ(message_details, message_details.last_nack_error_message + "_MaxRetriesExceeded");
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

void AdvancedConsumerTable::trimDeadLetterQueue() {
    if (m_config.dead_letter_retention_hours <= 0) {
        SWSS_LOG_INFO("AdvancedConsumerTable ('%s'): DLQ retention is disabled (retention hours <= 0).", getTableName().c_str());
        return;
    }
    if (!m_db) {
        SWSS_LOG_ERROR("AdvancedConsumerTable ('%s'): DBConnector (m_db) is null, cannot trim DLQ.", getTableName().c_str());
        return;
    }
    SWSS_LOG_INFO("AdvancedConsumerTable ('%s'): Starting DLQ trim. Retention: %d hours.", getTableName().c_str(), m_config.dead_letter_retention_hours);
    SWSS_LOG_WARN("AdvancedConsumerTable ('%s'): DLQ trim for HASH-based DLQ (where each DLQ'd message is a separate key) is inefficient. This current implementation is a placeholder and will not actually trim individual message keys from DLQ based on timestamp within the payload.", getTableName().c_str());
    // Placeholder: Actual trimming logic for HASH-based DLQ is complex and not implemented.
    // See comments in previous attempts for discussion on SCAN-based approach or alternative DLQ structures.
}

std::vector<common::Message> AdvancedConsumerTable::popMessagesForReplay(size_t count) {
    std::vector<common::Message> messages_for_replay;
    if (!m_db || count == 0) {
         SWSS_LOG_ERROR("AdvancedConsumerTable ('%s'): DBConnector (m_db) is null or count is 0, cannot pop messages from DLQ for replay.", getTableName().c_str());
        return messages_for_replay;
    }
    if (m_dlq_table_name.empty()) {
        SWSS_LOG_ERROR("AdvancedConsumerTable ('%s'): DLQ table name is not set. Cannot pop messages.", getTableName().c_str());
        return messages_for_replay;
    }
    SWSS_LOG_WARN("AdvancedConsumerTable ('%s'): popMessagesForReplay for HASH-based DLQ is a placeholder and currently non-functional. It needs a listing mechanism (like a ZSET of DLQ message IDs by time) to fetch oldest messages. This stub returns an empty vector.", getTableName().c_str());
    // Placeholder: Actual replay logic for HASH-based DLQ is complex and not implemented.
    // See comments in previous attempts for discussion.
    return messages_for_replay;
}

// Placeholder for glob_match function
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
            if (!glob_match(key_glob_filter->glob_pattern, msg.original_key)) {
                SWSS_LOG_DEBUG("AdvancedConsumerTable ('%s'): Message ID '%s' (Key: '%s') FAILED KeyGlobFilter (Pattern: '%s')",
                               getTableName().c_str(), msg.id.c_str(), msg.original_key.c_str(), key_glob_filter->glob_pattern.c_str());
                return false;
            }
            SWSS_LOG_DEBUG("AdvancedConsumerTable ('%s'): Message ID '%s' (Key: '%s') PASSED KeyGlobFilter (Pattern: '%s')",
                               getTableName().c_str(), msg.id.c_str(), msg.original_key.c_str(), key_glob_filter->glob_pattern.c_str());
        } else if (auto* json_path_filter = dynamic_cast<common::JsonPathFilterConfig*>(filter_conf_base.get())) {
            SWSS_LOG_DEBUG("AdvancedConsumerTable ('%s'): Applying JsonPathFilter (ID: %s, Path: '%s', Expected: '%s') to Message ID '%s' (Content: '%s')",
                getTableName().c_str(), json_path_filter->filter_id.c_str(), json_path_filter->json_path_expression.c_str(),
                json_path_filter->expected_value.c_str(), msg.id.c_str(), msg.content.c_str());
            std::vector<FieldValueTuple> fields;
            if (!Json::deserialize(msg.content, fields)) {
                SWSS_LOG_ERROR("AdvancedConsumerTable ('%s'): Failed to deserialize msg.content for JsonPathFilter on Message ID '%s'. Content: %s",
                               getTableName().c_str(), msg.id.c_str(), msg.content.c_str());
                return false;
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
    }
    if (m_custom_message_filter) {
        SWSS_LOG_DEBUG("AdvancedConsumerTable ('%s'): Applying custom message filter to Message ID '%s'", getTableName().c_str(), msg.id.c_str());
        if (!m_custom_message_filter(msg)) {
            SWSS_LOG_DEBUG("AdvancedConsumerTable ('%s'): Message ID '%s' FAILED custom message filter.", getTableName().c_str(), msg.id.c_str());
            return false;
        }
        SWSS_LOG_DEBUG("AdvancedConsumerTable ('%s'): Message ID '%s' PASSED custom message filter.", getTableName().c_str(), msg.id.c_str());
    }
    SWSS_LOG_DEBUG("AdvancedConsumerTable ('%s'): Message ID '%s' (Key: '%s') PASSED ALL filters.",
                   getTableName().c_str(), msg.id.c_str(), msg.original_key.c_str());
    return true;
}

} // namespace swss
>>>>>>> REPLACE
