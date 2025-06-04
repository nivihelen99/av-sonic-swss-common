#ifndef COMMON_ADVANCED_PRODUCER_TABLE_H_
#define COMMON_ADVANCED_PRODUCER_TABLE_H_

#include "common/producertable.h"
#include "common/advanced_pubsub_types.h"
#include "dbconnector.h" // For DBConnector
#include <string>
#include <vector>
#include <map> // May be needed later for ack tracking

namespace swss {

class AdvancedProducerTable : public ProducerTable {
public:
    // Constructor mirroring ProducerTable, taking DBConnector and tableName
    AdvancedProducerTable(DBConnector* db, const std::string& tableName)
        : ProducerTable(db, tableName), m_db(db), m_delivery_mode(common::DeliveryMode::AT_LEAST_ONCE) {}

    // Constructor mirroring ProducerTable, taking RedisPipeline and tableName
    // ProducerTable itself has constructors for (DBConnector*, tableName), (RedisPipeline*, tableName, bool)
    // We need to ensure we are calling the correct base constructor.
    // If ProducerTable has a constructor like ProducerTable(DBConnector* db, const std::string& tableName, bool buffered = true)
    // then our constructor should match.
    // Looking at common/producertable.h, ProducerTable has:
    // ProducerTable(DBConnector *db, const std::string &tableName);
    // ProducerTable(RedisPipeline *pipeline, const std::string &tableName, bool buffered = false);
    // So, we should provide both.

    AdvancedProducerTable(RedisPipeline* pipeline, const std::string& tableName, bool buffered = false)
        : ProducerTable(pipeline, tableName, buffered), m_db(nullptr), m_delivery_mode(common::DeliveryMode::AT_LEAST_ONCE) {
        // Note: If using RedisPipeline, m_db might be null.
        // This needs careful consideration if m_db is used directly for operations not covered by ProducerTable.
        // For now, assume ProducerTable handles all direct Redis interactions via its pipeline.
    }

    // New set method with priority and correlation_id
    // This is an overload, not an override, due to different parameters.
    void set(const std::string& key, const std::vector<FieldValueTuple>& values,
             int priority = 5, const std::string& correlation_id = "");

    // Make base class set methods available
    using ProducerTable::set;
    // Alternatively, we could provide pass-through versions if we need to intercept them.
    // For now, `using` is simpler.

    // Set the delivery mode for messages sent via this producer
    void setDeliveryMode(common::DeliveryMode mode);

    // Wait for acknowledgment of a message with a given correlation_id
    // Returns true if ack received within timeout, false otherwise.
    bool waitForAck(const std::string& correlation_id, int timeout_ms);

    // Flushes messages from the priority queue to Redis
    void flushPriorityQueue();

    // Destructor
    virtual ~AdvancedProducerTable() = default;

private:
    DBConnector* m_db; // Store DBConnector for general operations
    DBConnector* m_ack_db_connector; // DB connector for ACK tracking in Redis (can be same as m_db)
    common::DeliveryMode m_delivery_mode;
    common::PriorityQueue<common::Message> m_priority_queue;

    // Placeholder for tracking messages awaiting acknowledgment
    // std::map<std::string, SomeAckTrackingInfo> m_ack_tracker;

    // TODO: Implement queue overflow handling mechanism
    // e.g., drop oldest, drop lowest priority, block producer, etc.

    // TODO: Implement backpressure mechanism if the queue grows too large
};

} // namespace swss

#endif // COMMON_ADVANCED_PRODUCER_TABLE_H_
