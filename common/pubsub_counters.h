#pragma once
#include <string>
#include <vector> // Required by generated code in producer/consumer

namespace swss {
namespace PubSubCounters {

// COUNTERS_DB Table Name
const std::string COUNTERS_TABLE_NAME = "PUBSUB_COUNTERS";

// Producer Metrics
inline std::string producer_published_count(const std::string& tableName, int priority) {
    return tableName + ":priority:" + std::to_string(priority) + ":published";
}
inline std::string producer_queue_depth(const std::string& tableName) {
    return tableName + ":producer_queue_depth"; // Overall depth of m_priority_queue
}
inline std::string producer_ack_timeout(const std::string& tableName) { // waitForAck timeouts
    return tableName + ":wait_for_ack_timeouts";
}
// Could add producer_sent_to_redis_count if flush() is instrumented further

// Consumer Metrics
inline std::string consumer_consumed_count(const std::string& tableName, int priority) {
    return tableName + ":priority:" + std::to_string(priority) + ":consumed";
}
inline std::string consumer_outgoing_queue_depth(const std::string& tableName) {
    return tableName + ":consumer_outgoing_queue_depth"; // m_priority_queue_out
}
inline std::string consumer_ack_success(const std::string& tableName) {
    return tableName + ":ack_success";
}
inline std::string consumer_nack_total(const std::string& tableName) {
    return tableName + ":nack_total";
}
inline std::string consumer_nack_requeued(const std::string& tableName) {
    return tableName + ":nack_requeued";
}
inline std::string consumer_nack_to_dlq(const std::string& tableName) {
    return tableName + ":nack_to_dlq";
}
inline std::string consumer_dlq_sent(const std::string& tableName) { // Messages sent to DLQ by this consumer
    return tableName + ":dlq_sent";
}
inline std::string consumer_filter_match(const std::string& tableName) {
    return tableName + ":filter_match";
}
inline std::string consumer_filter_miss(const std::string& tableName) {
    return tableName + ":filter_miss";
}
inline std::string consumer_timeout_requeued(const std::string& tableName) { // Messages requeued due to timeout
    return tableName + ":timeout_requeued";
}
inline std::string consumer_timeout_to_dlq(const std::string& tableName) { // Messages sent to DLQ due to timeout+retries
    return tableName + ":timeout_to_dlq";
}


} // namespace PubSubCounters
} // namespace swss
