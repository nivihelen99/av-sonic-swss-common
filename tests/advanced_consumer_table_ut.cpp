#include "gtest/gtest.h"
#include "dbconnector.h"
// #include "consumerstatetable.h" // Base class, not directly used unless casting
#include "advancedconsumertable.h"
#include "advancedproducertable.h" // To produce messages for testing consumer
#include "common/advanced_pubsub_types.h"
#include "common/json.h"
#include "common/redisapi.h"
#include "common/selectableevent.h"
#include "common/select.h"
#include "common/pubsub_counters.h"
#include "common/counterstable.h"
#include <memory>
#include <vector>
#include <map>
#include <thread> // For testing timeouts and select
#include <chrono>
#include <algorithm> // for std::find_if, std::transform
#include <cctype>    // for std::tolower


// For direct redisCommand if needed by test utilities
#include <hiredis/hiredis.h>


using namespace swss;
using namespace common;

// Helper function to produce a message using AdvancedProducerTable
void produce_message(AdvancedProducerTable& producer, const std::string& key, int priority, const std::string& content_val = "v", const std::string& corr_id = "", DeliveryMode mode = DeliveryMode::AT_MOST_ONCE) {
    std::vector<FieldValueTuple> fv;
    fv.emplace_back("field", content_val);
    producer.setDeliveryMode(mode); // Set delivery mode on producer for the message
    producer.set(key, fv, priority, corr_id);
    producer.flushPriorityQueue();
}

// Helper to convert MessageStatus enum to string (already in AdvancedProducerTableTest, can be global)
// std::string messageStatusToString(MessageStatus status) { ... }


class AdvancedConsumerTableTest : public ::testing::Test {
protected:
    std::shared_ptr<DBConnector> m_db;         // For APPL_DB (main data)
    std::shared_ptr<DBConnector> m_stateDb;    // For STATE_DB (ACKs, status, counters)
    std::unique_ptr<AdvancedConsumerTable> m_consumer;
    std::unique_ptr<AdvancedProducerTable> m_producer; // To send messages for consumer to receive
    std::string m_tableName = "TEST_CONSUMER_TABLE";
    std::string m_dlqTableName; // Will be set in SetUp

    std::string m_ackStatusPrefix = Message::MSG_STATUS_PREFIX;
    std::string m_ackCorrIdsPrefix = Message::CORRELATION_IDS_PREFIX;
    std::string m_pendingMessagesKey = Message::PENDING_MESSAGES_BY_TIME_KEY;
    std::string m_countersDbTableName = PubSubCounters::COUNTERS_TABLE_NAME;
    std::string m_producerCountersQueueDepthKey;


    void connectToRedis() {
        const char* redis_hostname = std::getenv("REDIS_SERVER_ADDRESS");
        if (!redis_hostname) {
            redis_hostname = "127.0.0.1";
        }
        int redis_port = 6379;

        m_db = std::make_shared<DBConnector>(APPL_DB, redis_hostname, redis_port, 0);
        m_stateDb = std::make_shared<DBConnector>(STATE_DB, redis_hostname, redis_port, 0);

        // Clear data from previous tests
        // Note: DLQ table name is derived from m_tableName, so clear it after m_consumer is created or name is known.
    }

    // Helper to clear SCAN results
    void clearKeysByScan(DBConnector* db, const std::string& pattern) {
        RedisCommand scanCmd;
        std::string cursor = "0";
        do {
            scanCmd.format("SCAN %s MATCH %s COUNT 100", cursor.c_str(), pattern.c_str());
            RedisReply rScan(db, scanCmd, true); // Expect array
            auto scanResult = rScan.getScanReply();
            cursor = scanResult.first;
            for (const auto& key : scanResult.second) {
                RedisCommand delCmd;
                delCmd.format("DEL %s", key.c_str());
                RedisReply rDel(db, delCmd, false);
            }
        } while (cursor != "0");
    }


    void clearTableData() { // Clears main table and its DLQ
        RedisCommand delCmd;
        // Clear main table stream (assuming it's a stream key)
        delCmd.format("DEL %s", m_tableName.c_str());
        RedisReply rMain(m_db.get(), delCmd, false);

        // Clear DLQ table (ProducerTable creates keys like "DLQ_TABLE_NAME:KEY")
        // So we need to scan for these keys if DLQ is not a simple list.
        // Our current moveToDLQ uses m_dlq_producer->set(msg.id, ...),
        // where m_dlq_producer is for table m_dlqTableName.
        // ProducerTable forms keys as TABLE_NAME:KEY.
        std::string dlq_key_pattern = m_dlqTableName + TABLE_NAME_SEPARATOR + "*";
        clearKeysByScan(m_db.get(), dlq_key_pattern); // DLQ data is in APPL_DB via ProducerTable
    }

    void clearAckData() { // Clears from m_stateDb
        clearKeysByScan(m_stateDb.get(), m_ackStatusPrefix + "*");
        clearKeysByScan(m_stateDb.get(), m_ackCorrIdsPrefix + "*");

        RedisCommand delCmd;
        delCmd.format("DEL %s", m_pendingMessagesKey.c_str());
        RedisReply rDelPending(m_stateDb.get(), delCmd, false);
    }

    void clearCounters() { // Clears from m_stateDb (where CountersTable writes)
        RedisCommand delCmd;
        delCmd.format("DEL %s", m_countersDbTableName.c_str());
        RedisReply r(m_stateDb.get(), delCmd, false);
    }


    void SetUp() override {
        connectToRedis();
        // No ConfigDBConnector, so default PubSubConfig is used for both
        m_consumer = std::make_unique<AdvancedConsumerTable>(m_db.get(), m_tableName, DEFAULT_ADVANCED_POP_BATCH_SIZE, nullptr);
        m_producer = std::make_unique<AdvancedProducerTable>(m_db.get(), m_tableName, nullptr);
        m_dlqTableName = m_consumer->getDlqTableName();
        m_producerCountersQueueDepthKey = PubSubCounters::producer_queue_depth(m_tableName);


        // Clear data after all names are known
        clearTableData();
        clearAckData();
        clearCounters();
    }

    void TearDown() override {
        // Data clearing is done in SetUp to ensure clean state for each test
    }

    std::map<std::string, std::string> getAckStatus(const std::string& messageId) {
        std::map<std::string, std::string> status_map;
        std::string status_key = m_ackStatusPrefix + messageId;
        RedisCommand hgetallCmd;
        hgetallCmd.format("HGETALL %s", status_key.c_str());
        RedisReply r(m_stateDb.get(), hgetallCmd, true);
        r.getHash(status_map);
        return status_map;
    }

    // Check if a specific message ID exists in the DLQ (which is a set of HASHes)
    bool checkMsgInDLQ(const std::string& messageId) {
        std::string dlq_message_key = m_dlqTableName + TABLE_NAME_SEPARATOR + messageId;
        RedisCommand existsCmd;
        existsCmd.format("EXISTS %s", dlq_message_key.c_str());
        RedisReply r(m_db.get(), existsCmd, true); // DLQ data is in APPL_DB
        return r.getReply<long long>() == 1;
    }

    long long getCounterValue(const std::string& field_key) {
        std::string val_str;
        // CountersTable writes to STATE_DB as per its constructor in AdvancedConsumerTable
        RedisCommand hgetCmd;
        hgetCmd.format("HGET %s %s", m_countersDbTableName.c_str(), field_key.c_str());
        RedisReply r(m_stateDb.get(), hgetCmd, true);
        if (r.isNil() || r.getContext()->reply->str == nullptr) return 0;
        val_str = r.getReply<std::string>();
        if (val_str.empty()) return 0;
        return std::stoll(val_str);
    }

public:
    void verify_popped_message(const Message& msg, const std::string& expected_key, int expected_priority, const std::string& expected_val = "v", const std::string& expected_corr_id = "") {
        EXPECT_EQ(msg.original_key, expected_key);
        EXPECT_EQ(msg.priority, expected_priority);
        EXPECT_FALSE(msg.id.empty());
        if (!expected_corr_id.empty()) {
            EXPECT_EQ(msg.correlation_id, expected_corr_id);
        }
        Json content_json = Json::parse(msg.content);
        std::vector<FieldValueTuple> content_fv;
        Json::deserialize(content_json, content_fv); // Assumes swss::Json::deserialize static method
        ASSERT_EQ(content_fv.size(), 1);
        EXPECT_EQ(fvValue(content_fv[0]), expected_val);
    }
};

TEST_F(AdvancedConsumerTableTest, ConstructorAndDefaults) {
    EXPECT_NE(m_consumer, nullptr);
    EXPECT_TRUE(m_consumer->isRedisConnected());
}

TEST_F(AdvancedConsumerTableTest, PopInPriorityOrder) {
    produce_message(*m_producer, "key_low", 1);
    produce_message(*m_producer, "key_high", 9);
    produce_message(*m_producer, "key_mid", 5);

    std::deque<Message> messages;
    m_consumer->pops(messages, 3); // Request up to 3 messages
    ASSERT_EQ(messages.size(), 3);

    verify_popped_message(messages[0], "key_high", 9);
    verify_popped_message(messages[1], "key_mid", 5);
    verify_popped_message(messages[2], "key_low", 1);

    EXPECT_EQ(getCounterValue(PubSubCounters::consumer_consumed_count(m_tableName, 9)), 1);
    EXPECT_EQ(getCounterValue(PubSubCounters::consumer_consumed_count(m_tableName, 5)), 1);
    EXPECT_EQ(getCounterValue(PubSubCounters::consumer_consumed_count(m_tableName, 1)), 1);
    EXPECT_EQ(getCounterValue(PubSubCounters::consumer_outgoing_queue_depth(m_tableName)), 0);
}

TEST_F(AdvancedConsumerTableTest, ManualAckAndNackToDlq) {
    m_consumer->enableManualAck(true);
    produce_message(*m_producer, "key_ack_test", 5, "v_ack", "corr_ack_nack", DeliveryMode::AT_LEAST_ONCE);

    std::deque<Message> messages;
    m_consumer->pops(messages, 1);
    ASSERT_EQ(messages.size(), 1);
    Message msg1 = messages[0];
    verify_popped_message(msg1, "key_ack_test", 5, "v_ack", "corr_ack_nack");

    auto status1 = getAckStatus(msg1.id);
    EXPECT_EQ(status1[Message::MSG_STAT_FIELD_STATUS], "PENDING");

    m_consumer->ack(msg1.id);
    status1 = getAckStatus(msg1.id);
    EXPECT_EQ(status1[Message::MSG_STAT_FIELD_STATUS], "ACKED");
    EXPECT_EQ(getCounterValue(PubSubCounters::consumer_ack_success(m_tableName)), 1);

    produce_message(*m_producer, "key_nack_test", 6, "v_nack", "corr_ack_nack", DeliveryMode::AT_LEAST_ONCE);
    m_consumer->pops(messages, 1);
    ASSERT_EQ(messages.size(), 1);
    Message msg2 = messages[0];

    m_consumer->nack(msg2.id, false, "test nack reason for dlq"); // requeue=false
    auto status2 = getAckStatus(msg2.id);
    EXPECT_EQ(status2[Message::MSG_STAT_FIELD_STATUS], "NACKED");
    EXPECT_TRUE(checkMsgInDLQ(msg2.id));
    EXPECT_EQ(getCounterValue(PubSubCounters::consumer_nack_total(m_tableName)), 1);
    EXPECT_EQ(getCounterValue(PubSubCounters::consumer_nack_to_dlq(m_tableName)), 1);
    EXPECT_EQ(getCounterValue(PubSubCounters::consumer_dlq_sent(m_tableName)), 1);
}

TEST_F(AdvancedConsumerTableTest, NackWithRequeue) {
    m_consumer->enableManualAck(true);
    produce_message(*m_producer, "key_requeue", 7, "v_requeue", "corr_requeue", DeliveryMode::AT_LEAST_ONCE);
    std::deque<Message> messages;
    m_consumer->pops(messages, 1);
    ASSERT_EQ(messages.size(), 1);
    Message msg = messages[0];

    m_consumer->nack(msg.id, true, "requeue this");
    auto status = getAckStatus(msg.id);
    EXPECT_EQ(status[Message::MSG_STAT_FIELD_STATUS], "PENDING");
    EXPECT_EQ(status["retry_count"], "1");
    EXPECT_EQ(status["last_nack_reason"], "requeue this");
    EXPECT_EQ(getCounterValue(PubSubCounters::consumer_nack_total(m_tableName)), 1);
    EXPECT_EQ(getCounterValue(PubSubCounters::consumer_nack_requeued(m_tableName)), 1);
}


TEST_F(AdvancedConsumerTableTest, KeyGlobFilter) {
    m_consumer->setKeyFilter("accept_this:*"); // Match keys starting with "accept_this:"
    produce_message(*m_producer, "reject_this:key1", 1);
    produce_message(*m_producer, "accept_this:key2", 2);

    std::deque<Message> messages;
    m_consumer->pops(messages, 2); // Try to pop 2
    ASSERT_EQ(messages.size(), 1); // Should only get one
    verify_popped_message(messages[0], "accept_this:key2", 2);
    EXPECT_EQ(getCounterValue(PubSubCounters::consumer_filter_match(m_tableName)), 1);
    EXPECT_EQ(getCounterValue(PubSubCounters::consumer_filter_miss(m_tableName)), 1);
}

TEST_F(AdvancedConsumerTableTest, SelectableNotification) {
    Select s;
    SelectableEvent selEvent;
    m_consumer->enableManualAck(false); // Keep it simple for this test

    s.addSelectable(m_consumer.get()); // Add consumer to Select
    s.addSelectable(&selEvent);

    // Initially, no data
    int result = s.select(&selEvent, 100); // Timeout 100ms
    EXPECT_EQ(result, Select::OBJECT);
    EXPECT_TRUE(selEvent.isSet(s)); // Should be timeout
    EXPECT_FALSE(m_consumer->isSet(s));

    produce_message(*m_producer, "key_select", 1);

    // Now data should be available
    result = s.select(&selEvent, 1000);
    EXPECT_EQ(result, Select::OBJECT);
    EXPECT_FALSE(selEvent.isSet(s)); // Consumer should have data
    EXPECT_TRUE(m_consumer->isSet(s));

    std::deque<Message> messages;
    m_consumer->pops(messages, 1);
    ASSERT_EQ(messages.size(), 1);
    verify_popped_message(messages[0], "key_select", 1);

    // After popping, eventfd should be cleared, hasData should be false
    // (unless more data was fetched by processMessagesFromRedisToOutgoingQueue)
    // This part depends on exact timing of eventfd read vs hasData check
    // For now, just check if queue is empty
    EXPECT_TRUE(m_consumer->isSet(s)); // This might still be true if eventfd not cleared by select() itself
                                       // The consumer's readData() clears it.
    m_consumer->readData(); // Manually call to ensure eventfd is cleared
    EXPECT_FALSE(m_consumer->hasData()); // Now queue should be empty and eventfd cleared
}


// TODO: Add tests for:
// - ContentFilter (requires more complex message content setup)
// - CustomFilter
// - processTimedOutMessages (requires manipulating timestamps in PENDING_MESSAGES_BY_TIME_KEY and waiting)
// - trimDeadLetterQueue (once DLQ is list-based and trim is implemented, or with HASH based scan)
// - popMessagesForReplay (once DLQ is list-based and replay is implemented, or with HASH based scan)
// - Config loading effects on ack_timeout_ms, max_retries etc.

```
