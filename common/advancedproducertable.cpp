#include "common/advancedproducertable.h"
#include "common/json.h"
#include "common/logger.h" // For SWSS_LOG_WARN
#include <uuid/uuid.h>    // For UUID generation
#include <chrono>
#include <vector>
#include <string>
#include <utility> // For std::pair

// Helper to serialize common::Message to swss::Json
// This should ideally be in a common utility or alongside swss::Json definitions,
// or in common::Message itself if it had a toJson method.
// Placing it here for now to be self-contained for this file's needs.
namespace swss {
    // Assuming swss::Json is similar to nlohmann::json
    void to_json(Json& j, const common::Message& m) {
        j = Json{
            {"id", m.id},
            {"priority", m.priority},
            {"original_key", m.original_key},
            {"content", m.content}, // content is already a JSON string of original FieldValueTuples
            {"timestamp_ms", std::chrono::duration_cast<std::chrono::milliseconds>(m.timestamp.time_since_epoch()).count()},
            {"retry_count", m.retry_count},
            {"correlation_id", m.correlation_id},
            {"delivery_mode", static_cast<int>(m.delivery_mode)},
            {"original_table_name", m.original_table_name}, // Added original_table_name
            {"dlq_reason", m.dlq_reason},
            {"dlq_timestamp_ms", m.dlq_timestamp_ms},
            {"last_nack_error_message", m.last_nack_error_message}
        };
    }
} // namespace swss


namespace swss {

// Static counter for unique ID generation if UUID fails or as a fallback part
static std::atomic<uint64_t> message_id_counter(0);

// Helper to generate a unique message ID
std::string generate_unique_message_id() {
    uuid_t uuid;
    uuid_generate_random(uuid);
    char uuid_str[37];
    uuid_unparse_lower(uuid, uuid_str);
    return std::string(uuid_str);
    // Fallback or alternative:
    // auto now = std::chrono::system_clock::now();
    // auto nanos = std::chrono::duration_cast<std::chrono::nanoseconds>(now.time_since_epoch()).count();
    // return std::to_string(nanos) + "_" + std::to_string(message_id_counter++);
}

#include "common/pubsub_config_loader.h" // For loadPubSubConfig

// Constructors
AdvancedProducerTable::AdvancedProducerTable(DBConnector* db, const std::string& tableName, swss::ConfigDBConnector* configDb)
    : ProducerTable(db, tableName), m_db(db), m_ack_db_connector(db) {
    // m_config is default-initialized by its own default constructor.
    if (configDb) {
        swss::loadPubSubConfig(configDb, getTableName(), m_config);
    }
    m_delivery_mode = m_config.delivery_mode_enum; // Set default delivery mode from config
    if (m_db) { // m_counters_table requires a valid DBConnector
        m_counters_table = std::make_unique<swss::CountersTable>(m_db, swss::PubSubCounters::COUNTERS_TABLE_NAME);
        m_counters_producer_queue_depth_key = swss::PubSubCounters::producer_queue_depth(getTableName());
    } else {
        SWSS_LOG_WARN("AdvancedProducerTable for table '%s': DBConnector is null, PubSub counters will be disabled.", getTableName().c_str());
    }
    SWSS_LOG_INFO("AdvancedProducerTable for table '%s' initialized. Default delivery mode: %d",
                  getTableName().c_str(), static_cast<int>(m_delivery_mode));
}

AdvancedProducerTable::AdvancedProducerTable(RedisPipeline* pipeline, const std::string& tableName, bool buffered, swss::ConfigDBConnector* configDb)
    : ProducerTable(pipeline, tableName, buffered), m_db(nullptr), m_ack_db_connector(nullptr) {
    if (configDb) {
        swss::loadPubSubConfig(configDb, getTableName(), m_config);
    }
    m_delivery_mode = m_config.delivery_mode_enum;
    if (m_ack_db_connector == nullptr) {
        SWSS_LOG_WARN("AdvancedProducerTable for table '%s' initialized with RedisPipeline, ACK tracking will be disabled.", tableName.c_str());
    }
    // Counters are disabled if m_db is null (which it is with pipeline constructor)
    if (!m_db) {
        SWSS_LOG_WARN("AdvancedProducerTable for table '%s' (pipeline): DBConnector is null, PubSub counters will be disabled.", getTableName().c_str());
    }
    SWSS_LOG_INFO("AdvancedProducerTable for table '%s' (pipeline) initialized. Default delivery mode: %d",
                  getTableName().c_str(), static_cast<int>(m_delivery_mode));
}

// Destructor
AdvancedProducerTable::~AdvancedProducerTable() {
    // Future: Ensure any pending messages in m_priority_queue are flushed or handled
    // if required by the delivery mode guarantees. For now, default behavior is fine.
}

// New set method
void AdvancedProducerTable::set(const std::string& key,
                                const std::vector<FieldValueTuple>& values,
                                int priority,
                                const std::string& correlation_id) {
    common::Message message;
    message.original_key = key; // Store the original key
    message.id = generate_unique_message_id();
    message.priority = priority;
    message.correlation_id = correlation_id;
    message.delivery_mode = m_delivery_mode;
    message.timestamp = std::chrono::system_clock::now();
    message.retry_count = 0;
    message.original_table_name = this->getTableName(); // Set original_table_name

    // Serialize FieldValueTuple vector to a JSON string for content
    // This is a common way to store structured data in the message content.
    // Using nlohmann::json as an example, assuming common::Json is similar or wraps it.
    // If common::Json has a direct way to serialize FieldValueTuple, use that.
    // For now, let's construct a simple JSON string representation.
    // Swsscommon's Json class might not directly support vector<FieldValueTuple>.
    // We'll build it manually or assume a helper function.
    // For simplicity, we'll use a basic serialization. A robust solution might involve common::Json more deeply.

    std::vector<std::pair<std::string, std::string>> fv_pairs;
    for (const auto& fv : values) {
        fv_pairs.push_back({fv.first, fv.second});
    }
    message.content = Json::serialize(fv_pairs); // Assuming common::Json can serialize this.
                                                 // If not, this would need a more manual JSON construction:
                                                 // e.g., Json j; for(const auto& fv : values) j[fv.first] = fv.second; message.content = j.dump();


    // TODO: Placeholder for queue overflow check
    // if (m_priority_queue.size() >= MAX_QUEUE_SIZE) {
    //     SWSS_LOG_WARN("Priority queue overflow for table %s. Message with key %s might be dropped or handled by overflow strategy.",
    //                   getTableName().c_str(), key.c_str());
    //     // Implement overflow strategy (e.g., drop, block)
    //     return; // Or throw an exception
    // }

    m_priority_queue.push(message);
    if (m_counters_table) {
        m_counters_table->notification_producer.inc(swss::PubSubCounters::producer_published_count(getTableName(), message.priority));
        m_counters_table->notification_producer.set(m_counters_producer_queue_depth_key, std::to_string(m_priority_queue.size()));
    }

    // Note: Base ProducerTable::set is NOT called here.
    // Messages are queued and will be sent by a separate mechanism.
    SWSS_LOG_DEBUG("Queued message with key %s, priority %d, id %s. Queue depth: %zu",
                   message.original_key.c_str(), priority, message.id.c_str(), m_priority_queue.size());
}

// Set the delivery mode
void AdvancedProducerTable::setDeliveryMode(common::DeliveryMode mode) {
    m_delivery_mode = mode;
    SWSS_LOG_INFO("Delivery mode for table %s set to %d", getTableName().c_str(), static_cast<int>(mode));
}

// Wait for acknowledgment
bool AdvancedProducerTable::waitForAck(const std::string& correlation_id, int timeout_ms) {
    if (correlation_id.empty()) {
        SWSS_LOG_ERROR("waitForAck called with empty correlation_id for table %s.", getTableName().c_str());
        return false;
    }

    if (!m_ack_db_connector) {
        SWSS_LOG_ERROR("waitForAck for table %s (corr_id: %s) cannot proceed: ACK DB connector is not initialized.",
                       getTableName().c_str(), correlation_id.c_str());
        return false;
    }

    auto start_time = std::chrono::steady_clock::now();
    std::string corr_key = std::string(common::Message::CORRELATION_IDS_PREFIX) + correlation_id;
    int default_ack_timeout_seconds = 300; // 5 minutes, make this configurable later

    SWSS_LOG_DEBUG("waitForAck for table %s: Waiting for ACKs for correlation ID '%s' (timeout: %d ms)",
                   getTableName().c_str(), correlation_id.c_str(), timeout_ms);

    while (true) {
        auto current_time = std::chrono::steady_clock::now();
        auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(current_time - start_time).count();

        if (elapsed_ms >= timeout_ms) {
            SWSS_LOG_WARN("waitForAck for table %s: Timeout for correlation ID '%s' after %lld ms.",
                          getTableName().c_str(), correlation_id.c_str(), elapsed_ms);
            if (m_counters_table) {
                m_counters_table->notification_producer.inc(swss::PubSubCounters::producer_ack_timeout(getTableName()));
            }
            return false; // Timeout
        }

        std::vector<std::string> message_ids;
        try {
            message_ids = m_ack_db_connector->getRedisContext()->smembers(corr_key);
        } catch (const std::exception& e) {
            SWSS_LOG_ERROR("waitForAck for table %s: Exception reading members of %s: %s",
                getTableName().c_str(), corr_key.c_str(), e.what());
            return false; // Error accessing Redis
        }

        if (message_ids.empty()) {
            // This could mean:
            // 1. All messages were ACKed and their IDs removed from this set.
            // 2. No messages were ever associated with this correlation_id (or they expired).
            // To differentiate, we'd ideally check if the correlation_id was ever valid or if it still exists but is empty.
            // For now, if the set is empty, assume all associated messages are processed (ACKed or NACKed and handled).
            // A more robust check would be if the corr_key itself no longer exists.
            bool corr_key_exists = m_ack_db_connector->getRedisContext()->exists({corr_key})[0];
            if (!corr_key_exists) {
                 SWSS_LOG_INFO("waitForAck for table %s: Correlation ID key '%s' does not exist. Assuming all ACKs processed or ID invalid/expired.",
                                getTableName().c_str(), corr_key.c_str());
                return true; // All messages under this correlation ID are resolved or never existed.
            } else {
                // Correlation key exists but is empty. This implies messages might have been produced but not yet flushed,
                // or an issue occurred. Given the polling nature, this state might be transient.
                SWSS_LOG_DEBUG("waitForAck for table %s: Correlation ID key '%s' exists but is empty. Polling.",
                               getTableName().c_str(), corr_key.c_str());
            }
        }

        bool all_acked = true;
        if (!message_ids.empty()) { // Only check statuses if there are message IDs
            for (const auto& msg_id : message_ids) {
                std::string status_key = std::string(common::Message::MSG_STATUS_PREFIX) + msg_id;
                std::string status_str;
                try {
                     auto status_reply = m_ack_db_connector->getRedisContext()->hget(status_key, common::Message::MSG_STAT_FIELD_STATUS);
                     if (status_reply) {
                         status_str = *status_reply;
                     }
                } catch (const std::exception& e) {
                    SWSS_LOG_ERROR("waitForAck for table %s: Exception reading status for msg_id %s: %s",
                        getTableName().c_str(), msg_id.c_str(), e.what());
                    return false; // Error accessing Redis, can't confirm status
                }

                if (status_str.empty()) {
                    // Key or field might have expired or been improperly cleaned up.
                    SWSS_LOG_WARN("waitForAck for table %s: Status for message ID '%s' not found (key: %s). Assuming PENDING or error.",
                                  getTableName().c_str(), msg_id.c_str(), status_key.c_str());
                    all_acked = false; // Treat missing status as not ACKED.
                    break;
                } else if (status_str == ENUM_TO_STRING(common::MessageStatus::NACKED)) {
                    SWSS_LOG_WARN("waitForAck for table %s: Message ID '%s' was NACKED for correlation ID '%s'.",
                                  getTableName().c_str(), msg_id.c_str(), correlation_id.c_str());
                    return false; // One NACK means the batch associated with correlation_id failed.
                } else if (status_str != ENUM_TO_STRING(common::MessageStatus::ACKED)) {
                    all_acked = false; // Still PENDING or some other state
                    break;
                }
            }
        } else { // message_ids is empty
             // If corr_key still exists but is empty, means producer hasn't added msg_ids yet or they were all processed.
             // If it doesn't exist, means it's fully processed or invalid.
            if (m_ack_db_connector->getRedisContext()->exists({corr_key})[0]) {
                all_acked = false; // corr_key exists but no messages yet - effectively PENDING
            } else {
                all_acked = true; // corr_key gone, all processed.
            }
        }


        if (all_acked) {
            SWSS_LOG_INFO("waitForAck for table %s: All messages for correlation ID '%s' are ACKED.",
                          getTableName().c_str(), correlation_id.c_str());
            return true;
        }

        // Sleep before next poll
        std::this_thread::sleep_for(std::chrono::milliseconds(100)); // Configurable poll interval
    }
    // Should not be reached due to timeout check at the beginning of the loop.
    return false;
}

bool AdvancedProducerTable::isRedisConnected() const {
    if (!m_db || !m_db->getRedisContext()) {
        SWSS_LOG_ERROR("Redis context not available for PING.");
        return false;
    }
    redisReply *reply = static_cast<redisReply *>(redisCommand(m_db->getRedisContext()->getContext(), "PING"));
    if (reply == nullptr) {
        if (m_db->getRedisContext()->getContext() && m_db->getRedisContext()->getContext()->err) {
            SWSS_LOG_ERROR("Redis PING command failed: %s", m_db->getRedisContext()->getContext()->errstr);
        } else {
            SWSS_LOG_ERROR("Redis PING command failed (reply is null), possibly disconnected.");
        }
        return false;
    }
    bool is_ok = (reply->type == REDIS_REPLY_STATUS && strcmp(reply->str, "PONG") == 0);
    freeReplyObject(reply);
    if (!is_ok) {
        SWSS_LOG_WARN("Redis PING did not return PONG.");
    }
    return is_ok;
}

// Flushes messages from the priority queue to Redis
void AdvancedProducerTable::flushPriorityQueue() {
    if (m_priority_queue.empty()) {
        return;
    }
    SWSS_LOG_INFO("Flushing priority queue for table %s, current size: %zu", getTableName().c_str(), m_priority_queue.size());

    // Check connectivity before attempting to flush numerous messages
    if (!isRedisConnected()) {
        SWSS_LOG_ERROR("Cannot flush priority queue for table %s: Redis is not connected.", getTableName().c_str());
        // Depending on policy, messages could be re-queued or held. For now, they remain in m_priority_queue.
        return;
    }

    while (!m_priority_queue.empty()) {
        common::Message msg_to_send = m_priority_queue.pop();

        SWSS_LOG_DEBUG("Processing message ID %s, Key %s, Priority %d for table %s for actual sending.",
                       msg_to_send.id.c_str(), msg_to_send.original_key.c_str(), msg_to_send.priority, getTableName().c_str());

        std::string serialized_full_message;
        try {
            Json json_message;
            to_json(json_message, msg_to_send); // Uses the to_json function defined above
            serialized_full_message = json_message.dump(); // Or .serialize() depending on swss::Json API
        } catch (const std::exception& e) {
            SWSS_LOG_ERROR("Exception during full message serialization for ID %s, Key %s: %s. Skipping message.",
                           msg_to_send.id.c_str(), msg_to_send.original_key.c_str(), e.what());
            // TODO: Handle serialization failure (e.g., move to dead-letter queue or log and drop)
            // For now, if it can't be serialized, it's dropped from the queue and won't be sent.
            // Potentially re-queue with an error counter if this is transient.
            continue;
        }

        std::vector<FieldValueTuple> payload_values;
        payload_values.emplace_back(common::Message::MSG_PAYLOAD_FIELD, serialized_full_message);

        try {
            // Call base ProducerTable's set method to send to Redis
            // The key is the original message key. The value is a single field-value pair
            // where the field is MSG_PAYLOAD_FIELD and value is the serialized full message.
            ProducerTable::set(msg_to_send.original_key, payload_values);
            SWSS_LOG_INFO("Sent message ID %s, Key %s to Redis for table %s. Payload field: '%s'",
                          msg_to_send.id.c_str(), msg_to_send.original_key.c_str(), getTableName().c_str(), common::Message::MSG_PAYLOAD_FIELD);

            // After successful send, if reliable delivery is needed, record status.
            if (m_ack_db_connector &&
                (msg_to_send.delivery_mode == common::DeliveryMode::AT_LEAST_ONCE || msg_to_send.delivery_mode == common::DeliveryMode::EXACTLY_ONCE)) {

                std::string status_key = std::string(common::Message::MSG_STATUS_PREFIX) + msg_to_send.id;
                std::vector<std::pair<std::string, std::string>> status_fields;
                // Store status as string representation
                const char* status_str_val = "PENDING"; // Default, should not happen if logic is right
                if (common::MessageStatus::PENDING == common::MessageStatus::PENDING) status_str_val = "PENDING";
                // Add more if other initial states are possible, though PENDING is typical.

                status_fields.emplace_back(common::Message::MSG_STAT_FIELD_STATUS, status_str_val);
                status_fields.emplace_back(common::Message::MSG_STAT_FIELD_TIMESTAMP, std::to_string(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count()));
                if (!msg_to_send.correlation_id.empty()) {
                    status_fields.emplace_back(common::Message::MSG_STAT_FIELD_CORR_ID, msg_to_send.correlation_id);
                }
                status_fields.emplace_back(common::Message::MSG_STAT_FIELD_DELIVERY_MODE, std::to_string(static_cast<int>(msg_to_send.delivery_mode)));
                status_fields.emplace_back(common::Message::MSG_STAT_FIELD_TABLE_NAME, getTableName());
                long long current_timestamp_ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
                status_fields.emplace_back(common::Message::MSG_STAT_FIELD_TIMESTAMP, std::to_string(current_timestamp_ms));


                try {
                    m_ack_db_connector->getRedisContext()->hmset(status_key, status_fields.begin(), status_fields.end());

                    // Default expiry for PENDING messages, e.g., 5 minutes. Should be configurable.
                    // This timeout should align with PubSubConfig::ack_timeout_ms or be greater.
                    int default_status_key_expiry_seconds = 300;
                    m_ack_db_connector->getRedisContext()->expire(status_key, default_status_key_expiry_seconds);

                    // Add to sorted set for timeout processing
                    m_ack_db_connector->getRedisContext()->zadd(
                        common::Message::PENDING_MESSAGES_BY_TIME_KEY,
                        static_cast<double>(current_timestamp_ms), // Score is timestamp
                        msg_to_send.id
                    );
                    // Note: The PENDING_MESSAGES_BY_TIME_KEY sorted set itself doesn't need individual expiry here.
                    // Expired message IDs will be cleaned from it by processTimedOutMessages.

                    if (!msg_to_send.correlation_id.empty()) {
                        std::string corr_key = std::string(common::Message::CORRELATION_IDS_PREFIX) + msg_to_send.correlation_id;
                        m_ack_db_connector->getRedisContext()->sadd(corr_key, { msg_to_send.id });
                        m_ack_db_connector->getRedisContext()->expire(corr_key, default_status_key_expiry_seconds); // Also expire the set
                    }
                    SWSS_LOG_DEBUG("Recorded PENDING status for message ID %s (table %s), added to timeout processing set.", msg_to_send.id.c_str(), getTableName().c_str());
                } catch (const std::exception& e_ack) {
                     SWSS_LOG_ERROR("Exception recording PENDING status for message ID %s: %s", msg_to_send.id.c_str(), e_ack.what());
                }
            }

        } catch (const std::exception& e) {
            SWSS_LOG_ERROR("Exception during ProducerTable::set for message ID %s, Key %s: %s. Re-queuing message.",
                           msg_to_send.id.c_str(), msg_to_send.original_key.c_str(), e.what());
            // Basic re-queueing strategy (could be improved with retry counts, backoff)
            // Be cautious of infinite re-queue loops if the error is persistent.
            // For now, push back with original priority. May need to adjust retry_count.
            msg_to_send.retry_count++;
            // TODO: Add max retry limit
            // if(msg_to_send.retry_count < MAX_RETRIES) {
            //    m_priority_queue.push(msg_to_send); // Re-queue
            // } else {
            //    SWSS_LOG_ERROR("Message ID %s, Key %s reached max retries. Moving to DLQ or discarding.", msg_to_send.id.c_str(), msg_to_send.original_key.c_str());
            //    // Handle max retries (e.g., dead-letter queue)
            // }
            m_priority_queue.push(msg_to_send); // Simplistic re-queue for now
            // To prevent busy loop on persistent error, maybe break or add delay
            break; // Breaking loop for now if send fails, to avoid rapid re-queue and re-pop.
                   // A better approach would be a delayed re-queue or circuit breaker.
        }
    }
    SWSS_LOG_INFO("Finished flushing priority queue for table %s, remaining size: %zu", getTableName().c_str(), m_priority_queue.size());
}

} // namespace swss
