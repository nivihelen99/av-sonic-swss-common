#include "gtest/gtest.h"
#include "common/multinotificationconsumer.h"
#include "common/dbconnector.h"
#include "common/json.h"     // For JSon::writeJson
#include "common/redisapi.h"  // For direct redis commands
#include "common/table.h"     // For KeyOpFieldsValuesTuple, FieldValueTuple, FieldValueTuple
#include <unistd.h>      // For usleep

// Helper macros for ChannelKeyOpFieldsValuesTuple
#define ckfvChannel(X) std::get<0>(X)
#define ckfvKey(X) std::get<1>(X)
#define ckfvOp(X) std::get<2>(X)
#define ckfvFieldsValues(X) std::get<3>(X)

// Use APPL_DB for testing, common in sonic-swss-common tests
#define TEST_DB_CONNECTOR "APPL_DB"
#define TEST_DB_INSTANCE 0 // Typically 0 for APPL_DB

// Test fixture for MultiNotificationConsumer tests
class MultiNotificationConsumerTest : public ::testing::Test {
protected:
    swss::DBConnector m_db;

    MultiNotificationConsumerTest() : m_db(TEST_DB_CONNECTOR, TEST_DB_INSTANCE) {
        // You can add any setup that needs to be done once per test fixture
        // For example, clearing test channels if needed, but PUBLISH to unique channels is often better.
    }

    ~MultiNotificationConsumerTest() override {
        // Clean up resources if necessary
        // For instance, could flush specific keys or channels from Redis if they were uniquely named for the test.
        // However, for notification channels, messages are transient and don't persist like DB keys.
    }

    // Helper to publish a message
    void publishMessage(const std::string& channel, const std::string& op, const std::string& key, const std::vector<swss::FieldValueTuple>& fv_list) {
        std::string json_message;
        std::vector<swss::FieldValueTuple> fv_wrapper;
        fv_wrapper.emplace_back(op, key); // op/key is the first element
        for (const auto& fv_pair : fv_list) {
            fv_wrapper.push_back(fv_pair);
        }
        swss::JSon::writeJson(fv_wrapper, json_message);

        auto context = m_db.getContext();
        ASSERT_NE(context, nullptr);

        redisReply *reply = (redisReply *)redisCommand(context, "PUBLISH %s %s", channel.c_str(), json_message.c_str());
        ASSERT_NE(reply, nullptr) << "PUBLISH command failed for channel " << channel << ", message: " << json_message;
        // PUBLISH returns an integer reply (number of clients that received the message).
        // It's okay even if it's 0 (no active subscribers other than potentially itself if a client subscribes to its own publishes).
        // The key is that the command executed and the message is sent.
        ASSERT_EQ(reply->type, REDIS_REPLY_INTEGER) << "PUBLISH command did not return INTEGER reply. Reply was: " << reply->str;
        SWSS_LOG_DEBUG("Published to %s, %lld clients received.", channel.c_str(), reply->integer);
        freeReplyObject(reply);
    }
};


TEST_F(MultiNotificationConsumerTest, SubscribeAndPopMultipleMessages) {
    // 1. Define Test Channels and Data
    std::vector<std::string> test_channels = {"MULTI_NC_UT_CH1", "MULTI_NC_UT_CH2"}; // Unique names for test
    std::string op1 = "SET", data1 = "UT_KEY_CH1";
    std::vector<swss::FieldValueTuple> fv1 = {{"field_A", "value_A1"}};
    std::string op2 = "DEL", data2 = "UT_KEY_CH2";
    std::vector<swss::FieldValueTuple> fv2 = {{"field_B", "value_B2"}};

    // 2. Publish Messages
    publishMessage(test_channels[0], op1, data1, fv1);
    publishMessage(test_channels[1], op2, data2, fv2);

    // Give Redis a moment to process publications and for subscriber to pick them up.
    // This is often needed in pub/sub tests.
    usleep(100000); // 100ms, adjust if needed

    // 3. Instantiate MultiNotificationConsumer
    // Use a non-default popBatchSize to test that aspect as well, e.g., 1 to force multiple pops if needed, or default.
    swss::MultiNotificationConsumer consumer(&m_db, test_channels, 100, 5); // Priority 100, Pop batch size 5

    // 4. Consume and Verify Messages
    std::deque<swss::ChannelKeyOpFieldsValuesTuple> vckco_deque; // Changed type
    
    int attempts = 0;
    // Wait for messages. Peek should return > 0 if messages are queued or on the socket.
    // readData() inside peek or pops will move them from socket to queue.
    while (consumer.peek() <= 0 && attempts < 150) { // Try for up to ~1.5 seconds
        usleep(10000); // 10ms
        attempts++;
    }
    ASSERT_GT(consumer.peek(), 0) << "No data ready after " << attempts << " attempts.";

    consumer.pops(vckco_deque); // Changed deque
    
    // The current `pops` implementation tries to read more if batch size not met and data is on socket.
    // However, if messages arrive staggered or processing of first batch takes time,
    // a second peek/pops cycle might be needed for very specific test scenarios.
    // For this basic case, assume one `pops` after `peek` indicates data should be enough.
    if (vckco_deque.size() < 2 && consumer.peek() > 0) {
        SWSS_LOG_INFO("First pops got %zu messages, peeking for more.", vckco_deque.size());
        std::deque<swss::ChannelKeyOpFieldsValuesTuple> additional_vckco_deque; // Changed type
        consumer.pops(additional_vckco_deque);
        for(const auto& item : additional_vckco_deque) {
            vckco_deque.push_back(item);
        }
    }

    ASSERT_EQ(vckco_deque.size(), 2) << "Expected 2 messages, but received " << vckco_deque.size();

    bool found_msg1_on_correct_channel = false;
    bool found_msg2_on_correct_channel = false;

    for (const auto& kco : vckco_deque) { // Iterate over new deque
        std::string received_channel = ckfvChannel(kco);
        std::string received_op = ckfvOp(kco);
        std::string received_data_key = ckfvKey(kco);
        std::vector<swss::FieldValueTuple> received_fvt = ckfvFieldsValues(kco);

        SWSS_LOG_DEBUG("Processing received message from channel '%s': Key='%s', Op='%s'", 
                       received_channel.c_str(), received_data_key.c_str(), received_op.c_str());

        if (received_data_key == data1 && received_op == op1) {
            ASSERT_EQ(received_channel, test_channels[0]) << "Message 1 got from wrong channel";
            found_msg1_on_correct_channel = true;
            ASSERT_EQ(received_fvt.size(), fv1.size());
            if (!fv1.empty()) {
                 ASSERT_EQ(received_fvt[0].first, fv1[0].first);
                 ASSERT_EQ(received_fvt[0].second, fv1[0].second);
            }
        } else if (received_data_key == data2 && received_op == op2) {
            ASSERT_EQ(received_channel, test_channels[1]) << "Message 2 got from wrong channel";
            found_msg2_on_correct_channel = true;
            ASSERT_EQ(received_fvt.size(), fv2.size());
             if (!fv2.empty()) {
                 ASSERT_EQ(received_fvt[0].first, fv2[0].first);
                 ASSERT_EQ(received_fvt[0].second, fv2[0].second);
            }
        }
    }
    ASSERT_TRUE(found_msg1_on_correct_channel) << "Message 1 (Key: " << data1 << ", Op: " << op1 << ", Ch: " << test_channels[0] << ") not found or on wrong channel.";
    ASSERT_TRUE(found_msg2_on_correct_channel) << "Message 2 (Key: " << data2 << ", Op: " << op2 << ", Ch: " << test_channels[1] << ") not found or on wrong channel.";
}

// Add more tests here:
// - Test with empty channels list (constructor should throw)
// - Test with popBatchSize
// - Test peek behavior more thoroughly (data on socket vs data in queue)
// - Test timeout behavior if Selectable's select is used with it (might need another test setup)
// - Test multiple messages to the SAME channel
// - Test messages from one channel, then messages from another

TEST_F(MultiNotificationConsumerTest, ConstructorEmptyChannels) {
    std::vector<std::string> empty_channels;
    ASSERT_THROW(swss::MultiNotificationConsumer consumer(&m_db, empty_channels), std::invalid_argument);
}

TEST_F(MultiNotificationConsumerTest, PopBatchSizeTest) {
    std::vector<std::string> channels = {"BATCH_TEST_CH1", "BATCH_TEST_CH2", "BATCH_TEST_CH3"};
    
    publishMessage(channels[0], "SET", "BKEY1", {{"f", "v1"}});
    publishMessage(channels[1], "SET", "BKEY2", {{"f", "v2"}});
    publishMessage(channels[2], "SET", "BKEY3", {{"f", "v3"}});
    
    usleep(100000); // 100ms

    // Pop batch size of 2
    swss::MultiNotificationConsumer consumer(&m_db, channels, 100, 2);

    std::deque<swss::ChannelKeyOpFieldsValuesTuple> vckco_deque; // Changed type
    int attempts = 0;
    while(consumer.peek() <= 0 && attempts < 100) { usleep(10000); attempts++; }
    ASSERT_GT(consumer.peek(), 0);

    consumer.pops(vckco_deque); // Changed deque
    ASSERT_EQ(vckco_deque.size(), 2) << "Expected 2 messages due to popBatchSize = 2";

    // Verify channels for the first batch
    for (const auto& kco : vckco_deque) {
        std::string rcv_channel = ckfvChannel(kco);
        std::string rcv_key = ckfvKey(kco);
        bool found_channel = false;
        for(const auto& expected_ch : channels) { // Simple check: is it one of the subscribed channels?
            if (rcv_channel == expected_ch) {
                 // More specific check if key is tied to channel. BKEY1 -> CH1, BKEY2 -> CH2
                if ((rcv_key == "BKEY1" && rcv_channel == channels[0]) ||
                    (rcv_key == "BKEY2" && rcv_channel == channels[1]) ||
                    (rcv_key == "BKEY3" && rcv_channel == channels[2])) { // BKEY3 for the next pop
                     found_channel = true;
                     break;
                }
            }
        }
        ASSERT_TRUE(found_channel) << "Message " << rcv_key << " received on unexpected channel " << rcv_channel;
    }

    // There should be one message left
    ASSERT_GT(consumer.peek(), 0) << "Expected one message remaining in queue/socket";
    
    std::deque<swss::ChannelKeyOpFieldsValuesTuple> remaining_vckco_deque; // Changed type
    consumer.pops(remaining_vckco_deque); // Changed deque
    ASSERT_EQ(remaining_vckco_deque.size(), 1) << "Expected 1 more message";
    
    // Verify the last message's channel and key
    ASSERT_FALSE(remaining_vckco_deque.empty());
    const auto& last_kco = remaining_vckco_deque.front();
    std::string last_rcv_channel = ckfvChannel(last_kco);
    std::string last_rcv_key = ckfvKey(last_kco);
    ASSERT_EQ(last_rcv_key, "BKEY3");
    ASSERT_EQ(last_rcv_channel, channels[2]);
    
    ASSERT_LE(consumer.peek(), 0) << "Queue should be empty now"; // peek can be 0 or -1 if error, but should not be >0
}

// Test where multiple messages are sent to the same channel, mixed with other channels
TEST_F(MultiNotificationConsumerTest, MultipleMessagesSameChannel) {
    std::vector<std::string> test_channels = {"MULTI_SAME_CH1", "MULTI_SAME_CH2"};
    std::string op1 = "SET", data1_1 = "SAME_KEY1_CH1", data1_2 = "SAME_KEY2_CH1";
    std::vector<swss::FieldValueTuple> fv1_1 = {{"f", "v1_1"}}, fv1_2 = {{"f", "v1_2"}};
    std::string op2 = "SET", data2 = "OTHER_KEY_CH2";
    std::vector<swss::FieldValueTuple> fv2 = {{"f", "v2_other"}};

    publishMessage(test_channels[0], op1, data1_1, fv1_1);
    publishMessage(test_channels[1], op2, data2, fv2);
    publishMessage(test_channels[0], op1, data1_2, fv1_2); // Second message to CH1

    usleep(100000); // 100ms

    swss::MultiNotificationConsumer consumer(&m_db, test_channels);
    std::deque<swss::ChannelKeyOpFieldsValuesTuple> vckco_deque; // Changed type

    int attempts = 0;
    // Loop to ensure all messages are popped, respecting peek() and potential batching
    while(vckco_deque.size() < 3 && attempts < 200) { // Increased attempts for safety
        if (consumer.peek() > 0) {
            std::deque<swss::ChannelKeyOpFieldsValuesTuple> temp_deque;
            consumer.pops(temp_deque);
            for(const auto& item : temp_deque) {
                vckco_deque.push_back(item);
            }
        }
        if (vckco_deque.size() == 3) break; // Exit if all messages received
        usleep(10000); // 10ms poll
        attempts++;
    }
    
    ASSERT_EQ(vckco_deque.size(), 3) << "Expected 3 messages in total.";

    bool found_data1_1_ch1 = false;
    bool found_data1_2_ch1 = false;
    bool found_data2_ch2 = false;

    for (const auto& kco : vckco_deque) { // Iterate over new deque
        std::string rcv_channel = ckfvChannel(kco);
        std::string rcv_key = ckfvKey(kco);
        // std::string rcv_op = ckfvOp(kco); // op is 'SET' for all in this test

        if (rcv_key == data1_1) {
            ASSERT_EQ(rcv_channel, test_channels[0]);
            found_data1_1_ch1 = true;
        } else if (rcv_key == data1_2) {
            ASSERT_EQ(rcv_channel, test_channels[0]);
            found_data1_2_ch1 = true;
        } else if (rcv_key == data2) {
            ASSERT_EQ(rcv_channel, test_channels[1]);
            found_data2_ch2 = true;
        }
    }
    ASSERT_TRUE(found_data1_1_ch1) << "Message data1_1 on channel MULTI_SAME_CH1 not found or on wrong channel.";
    ASSERT_TRUE(found_data1_2_ch1) << "Message data1_2 on channel MULTI_SAME_CH1 not found or on wrong channel.";
    ASSERT_TRUE(found_data2_ch2) << "Message data2 on channel MULTI_SAME_CH2 not found or on wrong channel.";
}

// TODO: Consider a test for select() behavior if that's a primary use case,
// which would involve setting up a Select object and adding the consumer to it.
// For now, direct peek/pops tests cover the core functionality.
// Also, testing error conditions from Redis (e.g., connection drops) is harder
// in unit tests without a mock Redis or fault injection.
// The current tests assume a healthy Redis connection.
