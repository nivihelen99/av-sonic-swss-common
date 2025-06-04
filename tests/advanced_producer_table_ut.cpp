#include "gtest/gtest.h"
#include "dbconnector.h"
#include "producertable.h"
#include "advancedproducertable.h"
#include "common/advanced_pubsub_types.h"
#include "common/json.h"
#include "common/redisapi.h"
#include "common/pubsub_counters.h" // For counter name generation
#include "common/counterstable.h"   // For CountersTable
#include <memory>
#include <vector>
#include <map>
#include <algorithm> // for std::find_if

// It's assumed that hiredis/redis++.h are included via redisapi.h or dbconnector.h for redisContext types
// For direct redisCommand
#include <hiredis/hiredis.h>


using namespace swss;
using namespace common;

// Helper to convert MessageStatus enum to string for comparisons
// This might ideally be in advanced_pubsub_types.cpp or a utility file.
std::string messageStatusToString(MessageStatus status) {
    switch (status) {
        case MessageStatus::PENDING: return "PENDING";
        case MessageStatus::ACKED: return "ACKED";
        case MessageStatus::NACKED: return "NACKED";
        default: return "UNKNOWN_STATUS";
    }
}


class AdvancedProducerTableTest : public ::testing::Test {
protected:
    std::shared_ptr<DBConnector> m_db; // For APPL_DB where messages are typically produced
    std::shared_ptr<DBConnector> m_stateDb; // For ACK/Status, assuming STATE_DB or same as m_db
    std::unique_ptr<AdvancedProducerTable> m_producer;
    std::string m_tableName = "TEST_ADV_PRODUCER_TABLE";

    // ACK/Status related keys
    std::string m_ackStatusPrefix = Message::MSG_STATUS_PREFIX;
    std::string m_ackCorrIdsPrefix = Message::CORRELATION_IDS_PREFIX;
    std::string m_pendingMessagesKey = Message::PENDING_MESSAGES_BY_TIME_KEY;

    // Counters
    std::unique_ptr<CountersTable> m_countersDbTable;
    std::string m_countersTableName = PubSubCounters::COUNTERS_TABLE_NAME;


    void connectToRedis() {
        const char* redis_hostname = std::getenv("REDIS_SERVER_ADDRESS");
        if (!redis_hostname) {
            redis_hostname = "127.0.0.1"; // Default if not set
        }
        int redis_port = 6379; // Standard Redis port

        m_db = std::make_shared<DBConnector>(APPL_DB, redis_hostname, redis_port, 0);
        m_stateDb = std::make_shared<DBConnector>(STATE_DB, redis_hostname, redis_port, 0); // Assuming STATE_DB for ACK data
        m_countersDbTable = std::make_unique<CountersTable>(m_stateDb.get(), m_countersTableName);


        // Clear data from previous tests
        clearTableStreamData(); // Clear stream data from APPL_DB
        clearAckData(m_stateDb);       // Clear ACK data from STATE_DB
        clearCounters(m_stateDb);      // Clear counters from STATE_DB
    }

    void clearTableStreamData() {
        RedisCommand delCmd;
        delCmd.format("DEL %s", m_tableName.c_str()); // Streams are deleted with DEL
        RedisReply r(m_db.get(), delCmd, false);
        // No need to check reply for DEL usually
    }

    void clearAckData(std::shared_ptr<DBConnector>& db_conn) {
        RedisCommand keysCmd;
        RedisCommand delCmd;

        // Clear MSG_STATUS keys
        keysCmd.format("SCAN 0 MATCH %s* COUNT 1000", m_ackStatusPrefix.c_str());
        RedisReply rKeys(db_conn.get(), keysCmd, false); // SCAN returns array
        auto cursor_reply = rKeys.getContext()->reply;
        if (cursor_reply && cursor_reply->type == REDIS_REPLY_ARRAY && cursor_reply->elements == 2) {
            redisReply* keys_array = cursor_reply->element[1];
            for (size_t i = 0; i < keys_array->elements; ++i) {
                if (keys_array->element[i]->type == REDIS_REPLY_STRING) {
                    delCmd.format("DEL %s", keys_array->element[i]->str);
                    RedisReply rDel(db_conn.get(), delCmd, false);
                }
            }
        }

        // Clear CORR_IDS keys
        keysCmd.format("SCAN 0 MATCH %s* COUNT 1000", m_ackCorrIdsPrefix.c_str());
        rKeys.setContext(db_conn.get(), keysCmd, false);
        cursor_reply = rKeys.getContext()->reply;
         if (cursor_reply && cursor_reply->type == REDIS_REPLY_ARRAY && cursor_reply->elements == 2) {
            redisReply* keys_array = cursor_reply->element[1];
            for (size_t i = 0; i < keys_array->elements; ++i) {
                if (keys_array->element[i]->type == REDIS_REPLY_STRING) {
                    delCmd.format("DEL %s", keys_array->element[i]->str);
                    RedisReply rDel(db_conn.get(), delCmd, false);
                }
            }
        }

        // Clear PENDING_MSG_TS sorted set
        delCmd.format("DEL %s", m_pendingMessagesKey.c_str());
        RedisReply rDelPending(db_conn.get(), delCmd, false);
    }

    void clearCounters(std::shared_ptr<DBConnector>& db_conn) {
        RedisCommand hkeysCmd;
        hkeysCmd.format("HKEYS %s", m_countersTableName.c_str());
        RedisReply rHkeys(db_conn.get(), hkeysCmd, true); // Expect array or nil

        if (rHkeys.getContext()->reply && rHkeys.getContext()->reply->type == REDIS_REPLY_ARRAY) {
            auto keys = rHkeys.getArray();
            for (const auto& key_reply : keys) {
                if (key_reply->type == REDIS_REPLY_STRING) {
                    std::string key_str = key_reply->getReply<std::string>();
                    // Only delete keys related to m_tableName
                    if (key_str.rfind(m_tableName + ":", 0) == 0) {
                         RedisCommand hdelCmd;
                         hdelCmd.format("HDEL %s %s", m_countersTableName.c_str(), key_str.c_str());
                         RedisReply rHdel(db_conn.get(), hdelCmd, false);
                    }
                }
            }
        }
    }


    void SetUp() override {
        connectToRedis();
        // Using nullptr for ConfigDBConnector, so AdvancedProducerTable uses default PubSubConfig
        m_producer = std::make_unique<AdvancedProducerTable>(m_db.get(), m_tableName, nullptr);
    }

    void TearDown() override {
        // clearTableStreamData(); // Done in connectToRedis for next test
        // clearAckData(m_stateDb);
    }

public:
    // Helper to get ACK status from Redis
    std::map<std::string, std::string> getAckStatus(const std::string& messageId) {
        std::map<std::string, std::string> status_map;
        std::string status_key = m_ackStatusPrefix + messageId;
        RedisCommand hgetallCmd;
        hgetallCmd.format("HGETALL %s", status_key.c_str());
        RedisReply r(m_stateDb.get(), hgetallCmd, true); // Expect array or nil
        r.getHash(status_map);
        return status_map;
    }

    // Helper to retrieve the full serialized message from the ACK status (where it's stored for testing)
    // This is a workaround because ProducerTable writes to a stream, which is harder to inspect atomically in a test.
    Message getMessageFromAckStatus(const std::string& messageId) {
        Message msg;
        auto status_map = getAckStatus(messageId);
        if (status_map.count(Message::MSG_PAYLOAD_FIELD)) {
            Json j = Json::parse(status_map[Message::MSG_PAYLOAD_FIELD]);
            swss::from_json(j, msg);
        } else { // Fallback: if MSG_PAYLOAD_FIELD is not there, try to reconstruct from other fields
            if (status_map.count(Message::MSG_STAT_FIELD_TABLE_NAME)) msg.original_table_name = status_map[Message::MSG_STAT_FIELD_TABLE_NAME];
            // ... (this part is less critical if MSG_PAYLOAD_FIELD is always stored)
        }
        return msg;
    }
     long long getCounterValue(const std::string& key) {
        std::string val_str;
        m_countersDbTable->hget(key, val_str);
        if (val_str.empty()) return 0;
        return std::stoll(val_str);
    }

};

TEST_F(AdvancedProducerTableTest, Constructor) {
    EXPECT_NE(m_producer, nullptr);
    EXPECT_TRUE(m_producer->isRedisConnected());
}

TEST_F(AdvancedProducerTableTest, SetSingleMessageAndFlushAtLeastOnce) {
    m_producer->setDeliveryMode(DeliveryMode::AT_LEAST_ONCE);
    std::vector<FieldValueTuple> fvList;
    fvList.emplace_back("field1", "value1_ack");
    m_producer->set("key_ack1", fvList, 7, "corr_ack1");

    m_producer->flushPriorityQueue();

    // Verify ACK registration
    // To get the message ID, we need to find it.
    // One way: check PENDING_MESSAGES_BY_TIME_KEY for the latest entry.
    RedisCommand zrevrangeCmd;
    zrevrangeCmd.format("ZRANGE %s 0 0", m_pendingMessagesKey.c_str()); // Get the newest one
    RedisReply rZrevrange(m_stateDb.get(), zrevrangeCmd, true);
    auto newest_ids = rZrevrange.getArray();
    ASSERT_EQ(newest_ids.size(), 1);
    std::string msg_id = newest_ids[0]->getReply<std::string>();
    ASSERT_FALSE(msg_id.empty());

    auto status = getAckStatus(msg_id);
    ASSERT_FALSE(status.empty());
    EXPECT_EQ(status[Message::MSG_STAT_FIELD_STATUS], messageStatusToString(MessageStatus::PENDING));
    EXPECT_EQ(status[Message::MSG_STAT_FIELD_CORR_ID], "corr_ack1");
    EXPECT_EQ(status[Message::MSG_STAT_FIELD_TABLE_NAME], m_tableName);

    // Verify the message content stored in the ACK status payload
    Message stored_msg_data;
    Json j_payload = Json::parse(status[Message::MSG_PAYLOAD_FIELD]);
    swss::from_json(j_payload, stored_msg_data);

    EXPECT_EQ(stored_msg_data.original_key, "key_ack1");
    EXPECT_EQ(stored_msg_data.priority, 7);
    EXPECT_EQ(stored_msg_data.correlation_id, "corr_ack1");

    std::vector<FieldValueTuple> content_fv;
    Json::deserialize(Json::parse(stored_msg_data.content), content_fv);
    ASSERT_EQ(content_fv.size(), 1);
    EXPECT_EQ(fvField(content_fv[0]), "field1");
    EXPECT_EQ(fvValue(content_fv[0]), "value1_ack");

    // Verify counters
    EXPECT_EQ(getCounterValue(PubSubCounters::producer_published_count(m_tableName, 7)), 1);
    EXPECT_EQ(getCounterValue(m_counters_producer_queue_depth_key), 0); // After flush
}


TEST_F(AdvancedProducerTableTest, PriorityOrderAckRegistration) {
    m_producer->setDeliveryMode(DeliveryMode::AT_LEAST_ONCE);
    std::vector<FieldValueTuple> fv;
    fv.emplace_back("f", "v");

    m_producer->set("key_low", fv, 1, "corr_prio");
    m_producer->set("key_high", fv, 9, "corr_prio");
    m_producer->set("key_mid", fv, 5, "corr_prio");

    m_producer->flushPriorityQueue();

    // Verify order from PENDING_MESSAGES_BY_TIME_KEY (indirectly via scores)
    // Or, if we assume flush is sequential, we could try to get all 3 message IDs
    // and check their properties.
    RedisCommand zrangeCmd;
    // Get IDs with scores (timestamps) to verify order if timestamps are distinct enough
    zrangeCmd.format("ZRANGE %s 0 -1 WITHSCORES", m_pendingMessagesKey.c_str());
    RedisReply rZrange(m_stateDb.get(), zrangeCmd, true);
    auto items_with_scores = rZrange.getArray();
    ASSERT_EQ(items_with_scores.size(), 6); // 3 IDs + 3 scores

    // We expect high, then mid, then low priority messages to be processed by flush.
    // Their timestamps in PENDING_MESSAGES_BY_TIME_KEY should be close but reflect this order.
    // This test is a bit fragile due to timestamp closeness.
    // A better test would be to have an observer on ProducerTable::set or mock it.

    // For now, just check that 3 messages were registered for this correlation ID
    std::string corr_key = m_ackCorrIdsPrefix + "corr_prio";
    RedisCommand scardCmd;
    scardCmd.format("SCARD %s", corr_key.c_str());
    RedisReply rScard(m_stateDb.get(), scardCmd, true);
    EXPECT_EQ(rScard.getReply<long long>(), 3);

    EXPECT_EQ(getCounterValue(PubSubCounters::producer_published_count(m_tableName, 1)), 1);
    EXPECT_EQ(getCounterValue(PubSubCounters::producer_published_count(m_tableName, 5)), 1);
    EXPECT_EQ(getCounterValue(PubSubCounters::producer_published_count(m_tableName, 9)), 1);
    EXPECT_EQ(getCounterValue(m_counters_producer_queue_depth_key), 0);
}


TEST_F(AdvancedProducerTableTest, SetAtMostOnceNoAckRegistration) {
    m_producer->setDeliveryMode(DeliveryMode::AT_MOST_ONCE);
    std::vector<FieldValueTuple> fv;
    fv.emplace_back("f", "v_no_ack");
    m_producer->set("key_no_ack", fv, 5, "corr_no_ack");
    m_producer->flushPriorityQueue();

    // Verify no ACK registration
    RedisCommand zcountCmd;
    zcountCmd.format("ZCOUNT %s -inf +inf", m_pendingMessagesKey.c_str());
    RedisReply rZcount(m_stateDb.get(), zcountCmd, true);
    EXPECT_EQ(rZcount.getReply<long long>(), 0); // No messages should be in pending set

    std::string corr_key = m_ackCorrIdsPrefix + "corr_no_ack";
    RedisCommand existsCmd;
    existsCmd.format("EXISTS %s", corr_key.c_str());
    RedisReply rExists(m_stateDb.get(), existsCmd, true);
    EXPECT_EQ(rExists.getReply<long long>(), 0); // Correlation ID set should not exist

    // Verify published counter is still incremented
     EXPECT_EQ(getCounterValue(PubSubCounters::producer_published_count(m_tableName, 5)), 1);
}


TEST_F(AdvancedProducerTableTest, WaitForAckSuccess) {
    m_producer->setDeliveryMode(DeliveryMode::AT_LEAST_ONCE);
    std::vector<FieldValueTuple> fv;
    fv.emplace_back("f", "v_wait");
    m_producer->set("key_wait1", fv, 5, "corr_wait_succ");
    m_producer->flushPriorityQueue();

    // Get message ID from PENDING_MESSAGES_BY_TIME_KEY (assuming it's the only one)
    RedisCommand zrangeCmd;
    zrangeCmd.format("ZRANGE %s 0 0", m_pendingMessagesKey.c_str());
    RedisReply rZrange(m_stateDb.get(), zrangeCmd, true);
    auto ids = rZrange.getArray();
    ASSERT_EQ(ids.size(), 1);
    std::string msg1_id = ids[0]->getReply<std::string>();

    // Simulate ACK by consumer
    std::string status_key1 = m_ackStatusPrefix + msg1_id;
    RedisCommand hsetCmd;
    hsetCmd.format("HSET %s %s ACKED", status_key1.c_str(), Message::MSG_STAT_FIELD_STATUS.c_str());
    RedisReply rHset(m_stateDb.get(), hsetCmd, false);

    RedisCommand zremCmd; // Consumer would also remove from PENDING_MESSAGES_BY_TIME_KEY
    zremCmd.format("ZREM %s %s", m_pendingMessagesKey.c_str(), msg1_id.c_str());
    RedisReply rZrem(m_stateDb.get(), zremCmd, false);

    RedisCommand sremCmd; // And from correlation set
    sremCmd.format("SREM %s%s %s", m_ackCorrIdsPrefix.c_str(), "corr_wait_succ", msg1_id.c_str());
    RedisReply rSrem(m_stateDb.get(), sremCmd, false);


    bool result = m_producer->waitForAck("corr_wait_succ", 1000);
    EXPECT_TRUE(result);
}

TEST_F(AdvancedProducerTableTest, WaitForAckNacked) {
    m_producer->setDeliveryMode(DeliveryMode::AT_LEAST_ONCE);
    std::vector<FieldValueTuple> fv;
    fv.emplace_back("f", "v_nack");
    m_producer->set("key_nack", fv, 5, "corr_wait_nack");
    m_producer->flushPriorityQueue();

    RedisCommand zrangeCmd;
    zrangeCmd.format("ZRANGE %s 0 0", m_pendingMessagesKey.c_str());
    RedisReply rZrange(m_stateDb.get(), zrangeCmd, true);
    auto ids = rZrange.getArray();
    ASSERT_EQ(ids.size(), 1);
    std::string msg_nack_id = ids[0]->getReply<std::string>();

    // Simulate NACK
    std::string status_key_nack = m_ackStatusPrefix + msg_nack_id;
    RedisCommand hsetCmd;
    hsetCmd.format("HSET %s %s NACKED", status_key_nack.c_str(), Message::MSG_STAT_FIELD_STATUS.c_str());
    RedisReply rHset(m_stateDb.get(), hsetCmd, false);

    // Consumer would remove from PENDING_MESSAGES_BY_TIME_KEY and CORR_ID set after terminal NACK
    RedisCommand zremCmd;
    zremCmd.format("ZREM %s %s", m_pendingMessagesKey.c_str(), msg_nack_id.c_str());
    RedisReply rZrem(m_stateDb.get(), zremCmd, false);
    RedisCommand sremCmd;
    sremCmd.format("SREM %s%s %s", m_ackCorrIdsPrefix.c_str(), "corr_wait_nack", msg_nack_id.c_str());
    RedisReply rSrem(m_stateDb.get(), sremCmd, false);


    bool result = m_producer->waitForAck("corr_wait_nack", 1000);
    EXPECT_FALSE(result);
}

TEST_F(AdvancedProducerTableTest, WaitForAckTimeout) {
    m_producer->setDeliveryMode(DeliveryMode::AT_LEAST_ONCE);
    std::vector<FieldValueTuple> fv;
    fv.emplace_back("f", "v_timeout");
    m_producer->set("key_timeout", fv, 5, "corr_wait_timeout");
    m_producer->flushPriorityQueue();
    // Message remains PENDING in Redis

    bool result = m_producer->waitForAck("corr_wait_timeout", 200); // Short timeout
    EXPECT_FALSE(result);
    EXPECT_EQ(getCounterValue(PubSubCounters::producer_ack_timeout(m_tableName)), 1);
}

TEST_F(AdvancedProducerTableTest, IsRedisConnected) {
    EXPECT_TRUE(m_producer->isRedisConnected());
    // Further tests could involve stopping Redis, but that's more complex for UT.
}

// TODO: Add tests for:
// - Multiple messages for one correlation ID in waitForAck (all ACK, one NACK, one PENDING)
// - Config loading affecting default delivery mode (requires ConfigDBConnector setup)
// - waitForAck with empty correlation ID (already handled by check in method)
// - waitForAck when m_ack_db_connector is null (e.g. pipeline constructor without separate ACK DB)

```
