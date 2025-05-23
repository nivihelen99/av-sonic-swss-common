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

TEST_F(MultiNotificationConsumerTest, UnsubscribeSingleChannel) {
    std::vector<std::string> channels = {"US_CH1_S", "US_CH2_S", "US_CH3_S"};
    swss::MultiNotificationConsumer consumer(&m_db, channels);

    // Initial messages to make sure subscriptions are active
    publishMessage(channels[0], "INIT_OP", "INIT_KEY_CH1", {{"f", "v"}});
    publishMessage(channels[1], "INIT_OP", "INIT_KEY_CH2", {{"f", "v"}});
    publishMessage(channels[2], "INIT_OP", "INIT_KEY_CH3", {{"f", "v"}});
    usleep(100000); // 100ms

    std::deque<swss::ChannelKeyOpFieldsValuesTuple> received_msgs;
    int initial_peek = consumer.peek();
    if (initial_peek > 0) consumer.pops(received_msgs);
    ASSERT_EQ(received_msgs.size(), 3) << "Should receive initial 3 messages";
    received_msgs.clear();

    // Unsubscribe from the middle channel
    consumer.unsubscribe({channels[1]}); // Unsubscribe US_CH2_S
    usleep(100000); // Allow time for unsubscribe to process by Redis & client

    // New messages after unsubscribe
    publishMessage(channels[0], "NEXT_OP", "NEXT_KEY_CH1", {{"f", "vN1"}}); // Should be received
    publishMessage(channels[1], "NEXT_OP", "NEXT_KEY_CH2", {{"f", "vN2"}}); // Should NOT be received
    publishMessage(channels[2], "NEXT_OP", "NEXT_KEY_CH3", {{"f", "vN3"}}); // Should be received
    usleep(100000);

    int peek_val = consumer.peek();
    if (peek_val > 0) {
        consumer.pops(received_msgs);
    }
    
    // Might have received 0, 1, or 2 messages. Should not be 3.
    ASSERT_LE(received_msgs.size(), (size_t)2);

    bool found_ch1_next = false;
    bool found_ch2_next = false; // This should remain false
    bool found_ch3_next = false;

    for (const auto& msg_tuple : received_msgs) {
        const std::string& chan = ckfvChannel(msg_tuple);
        const std::string& key = ckfvKey(msg_tuple);
        if (chan == channels[0] && key == "NEXT_KEY_CH1") found_ch1_next = true;
        if (chan == channels[1] && key == "NEXT_KEY_CH2") found_ch2_next = true;
        if (chan == channels[2] && key == "NEXT_KEY_CH3") found_ch3_next = true;
    }

    ASSERT_TRUE(found_ch1_next) << "Did not find message from " << channels[0];
    ASSERT_FALSE(found_ch2_next) << "Found message from unsubscribed channel " << channels[1];
    ASSERT_TRUE(found_ch3_next) << "Did not find message from " << channels[2];

    // Final check: publish only to CH2 and ensure nothing is popped
    received_msgs.clear();
    publishMessage(channels[1], "FINAL_OP", "FINAL_KEY_CH2", {{"f", "vF"}});
    usleep(100000);
    peek_val = consumer.peek();
    if (peek_val > 0) { // Should be 0 if only CH2 was published to and we are unsubscribed
        consumer.pops(received_msgs);
    }
    ASSERT_TRUE(received_msgs.empty()) << "Received messages when only unsubscribed channel had new data.";
}

TEST_F(MultiNotificationConsumerTest, UnsubscribeMultipleChannels) {
    std::vector<std::string> channels = {"US_CH1_M", "US_CH2_M", "US_CH3_M"};
    swss::MultiNotificationConsumer consumer(&m_db, channels);

    publishMessage(channels[0], "INIT_OP", "INIT_KEY_CH1M", {});
    publishMessage(channels[1], "INIT_OP", "INIT_KEY_CH2M", {});
    publishMessage(channels[2], "INIT_OP", "INIT_KEY_CH3M", {});
    usleep(100000);
    std::deque<swss::ChannelKeyOpFieldsValuesTuple> received_msgs;
    if(consumer.peek() > 0) consumer.pops(received_msgs);
    ASSERT_EQ(received_msgs.size(), 3);
    received_msgs.clear();

    // Unsubscribe from US_CH2_M and US_CH3_M
    consumer.unsubscribe({channels[1], channels[2]});
    usleep(100000);

    publishMessage(channels[0], "NEXT_OP", "NEXT_KEY_CH1M", {}); // Should be received
    publishMessage(channels[1], "NEXT_OP", "NEXT_KEY_CH2M", {}); // Should NOT be received
    publishMessage(channels[2], "NEXT_OP", "NEXT_KEY_CH3M", {}); // Should NOT be received
    usleep(100000);

    if(consumer.peek() > 0) consumer.pops(received_msgs);
    
    ASSERT_LE(received_msgs.size(), (size_t)1);

    bool found_ch1_next = false;
    bool found_ch2_next = false;
    bool found_ch3_next = false;

    for (const auto& msg_tuple : received_msgs) {
        const std::string& chan = ckfvChannel(msg_tuple);
        const std::string& key = ckfvKey(msg_tuple);
        if (chan == channels[0] && key == "NEXT_KEY_CH1M") found_ch1_next = true;
        if (chan == channels[1] && key == "NEXT_KEY_CH2M") found_ch2_next = true;
        if (chan == channels[2] && key == "NEXT_KEY_CH3M") found_ch3_next = true;
    }

    ASSERT_TRUE(found_ch1_next);
    ASSERT_FALSE(found_ch2_next);
    ASSERT_FALSE(found_ch3_next);

    // Final check
    received_msgs.clear();
    publishMessage(channels[1], "FINAL_OP", "FINAL_KEY_CH2M", {});
    publishMessage(channels[2], "FINAL_OP", "FINAL_KEY_CH3M", {});
    usleep(100000);
    if(consumer.peek() > 0) consumer.pops(received_msgs);
    ASSERT_TRUE(received_msgs.empty());
}

TEST_F(MultiNotificationConsumerTest, UnsubscribeNonSubscribedChannel) {
    std::vector<std::string> channels = {"US_CH1_N", "US_CH2_N"};
    swss::MultiNotificationConsumer consumer(&m_db, channels);

    publishMessage(channels[0], "INIT_OP", "INIT_KEY_CH1N", {});
    publishMessage(channels[1], "INIT_OP", "INIT_KEY_CH2N", {});
    usleep(100000);
    std::deque<swss::ChannelKeyOpFieldsValuesTuple> received_msgs;
    if(consumer.peek() > 0) consumer.pops(received_msgs);
    ASSERT_EQ(received_msgs.size(), 2);
    received_msgs.clear();

    // Unsubscribe from a non-existent channel and one existing + one non-existent
    consumer.unsubscribe({"NON_EXISTENT_CHANNEL"});
    usleep(50000); // Shorter delay, as Redis won't do much
    consumer.unsubscribe({channels[0], "NON_EXISTENT_CHANNEL_2"}); // Unsubscribe from CH1
    usleep(100000);


    publishMessage(channels[0], "NEXT_OP", "NEXT_KEY_CH1N", {}); // Should NOT be received
    publishMessage(channels[1], "NEXT_OP", "NEXT_KEY_CH2N", {}); // Should be received
    usleep(100000);

    if(consumer.peek() > 0) consumer.pops(received_msgs);
    
    ASSERT_LE(received_msgs.size(), (size_t)1); // Only CH2 message should come

    bool found_ch1_next = false;
    bool found_ch2_next = false;

    for (const auto& msg_tuple : received_msgs) {
        const std::string& chan = ckfvChannel(msg_tuple);
        const std::string& key = ckfvKey(msg_tuple);
        if (chan == channels[0] && key == "NEXT_KEY_CH1N") found_ch1_next = true;
        if (chan == channels[1] && key == "NEXT_KEY_CH2N") found_ch2_next = true;
    }

    ASSERT_FALSE(found_ch1_next) << "Message from " << channels[0] << " (which should be unsubscribed) was received.";
    ASSERT_TRUE(found_ch2_next) << "Message from " << channels[1] << " (which should still be subscribed) was not received.";
}

TEST_F(MultiNotificationConsumerTest, UnsubscribeAllChannels) {
    std::vector<std::string> channels = {"US_CH1_ALL", "US_CH2_ALL"};
    swss::MultiNotificationConsumer consumer(&m_db, channels);

    publishMessage(channels[0], "INIT_OP", "INIT_KEY_CH1A", {});
    publishMessage(channels[1], "INIT_OP", "INIT_KEY_CH2A", {});
    usleep(100000);
    std::deque<swss::ChannelKeyOpFieldsValuesTuple> received_msgs;
    if(consumer.peek() > 0) consumer.pops(received_msgs);
    ASSERT_EQ(received_msgs.size(), 2);
    received_msgs.clear();

    // Unsubscribe from all channels
    consumer.unsubscribe({channels[0], channels[1]});
    usleep(100000);

    publishMessage(channels[0], "NEXT_OP", "NEXT_KEY_CH1A", {}); // Should NOT be received
    publishMessage(channels[1], "NEXT_OP", "NEXT_KEY_CH2A", {}); // Should NOT be received
    usleep(100000);

    // Peek should return 0 (no data) or possibly -1 (error, but less likely here)
    // It should not be positive.
    int peek_val = consumer.peek();
    ASSERT_LE(peek_val, 0) << "Peek returned > 0 after unsubscribing from all channels and publishing new messages.";

    consumer.pops(received_msgs); // Attempt to pop
    ASSERT_TRUE(received_msgs.empty()) << "Received messages after unsubscribing from all channels.";
}

TEST_F(MultiNotificationConsumerTest, PopPayload) {
    std::string channel = "PL_CH1";
    std::string op = "SET", key = "PayloadKey";
    std::vector<swss::FieldValueTuple> fv = {{"f1", "v1"}, {"f2", "v2"}};
    
    std::string expected_payload_json;
    std::vector<swss::FieldValueTuple> fv_wrapper_json; // For JSon::writeJson
    fv_wrapper_json.emplace_back(op, key);
    for(const auto& pair : fv) fv_wrapper_json.push_back(pair);
    swss::JSon::writeJson(fv_wrapper_json, expected_payload_json);

    swss::MultiNotificationConsumer consumer(&m_db, {channel});
    publishMessage(channel, op, key, fv);
    usleep(100000); // Allow message reception

    ASSERT_GT(consumer.peek(), 0) << "No data ready for PopPayload.";
    std::string received_payload = consumer.popPayload();
    ASSERT_EQ(received_payload, expected_payload_json);

    // Queue should be empty now
    ASSERT_LE(consumer.peek(), 0) << "Queue not empty after PopPayload.";

    // Test exception on empty queue
    EXPECT_THROW({
        try {
            consumer.popPayload();
        } catch (const std::runtime_error& e) {
            EXPECT_STREQ("Notification queue is empty, can't pop payload", e.what());
            throw; // Re-throw to satisfy EXPECT_THROW
        }
    }, std::runtime_error);
}

TEST_F(MultiNotificationConsumerTest, PopWithChannel) {
    std::string channel = "PWC_CH1";
    std::string op = "DEL", key = "PWCKey";
    std::vector<swss::FieldValueTuple> fv = {{"field_pwc", "value_pwc"}};

    std::string expected_payload_json;
    std::vector<swss::FieldValueTuple> fv_wrapper_json;
    fv_wrapper_json.emplace_back(op, key);
    for(const auto& pair : fv) fv_wrapper_json.push_back(pair);
    swss::JSon::writeJson(fv_wrapper_json, expected_payload_json);

    swss::MultiNotificationConsumer consumer(&m_db, {channel});
    publishMessage(channel, op, key, fv);
    usleep(100000);

    ASSERT_GT(consumer.peek(), 0) << "No data ready for PopWithChannel.";
    std::pair<std::string, std::string> result = consumer.popWithChannel();
    
    ASSERT_EQ(result.first, channel);
    ASSERT_EQ(result.second, expected_payload_json);

    ASSERT_LE(consumer.peek(), 0) << "Queue not empty after PopWithChannel.";
    EXPECT_THROW({
        try {
            consumer.popWithChannel();
        } catch (const std::runtime_error& e) {
            EXPECT_STREQ("Notification queue is empty, can't pop with channel", e.what());
            throw; // Re-throw
        }
    }, std::runtime_error);
}

TEST_F(MultiNotificationConsumerTest, PopOverloadBackwardCompatibility) {
    std::string channel = "BC_POP_CH1";
    std::string expected_op = "SET_BC", expected_key = "BC_KeyPop";
    std::vector<swss::FieldValueTuple> expected_fv = {{"f_bc", "v_bc"}};

    swss::MultiNotificationConsumer consumer(&m_db, {channel});
    publishMessage(channel, expected_op, expected_key, expected_fv);
    usleep(100000);

    ASSERT_GT(consumer.peek(), 0) << "No data ready for backward-compatible Pop.";
    std::string op_out, data_out;
    std::vector<swss::FieldValueTuple> fv_out;
    consumer.pop(op_out, data_out, fv_out); // Call the overload

    ASSERT_EQ(op_out, expected_op);
    ASSERT_EQ(data_out, expected_key);
    ASSERT_EQ(fv_out.size(), expected_fv.size());
    if (!expected_fv.empty()) {
        ASSERT_EQ(fv_out[0].first, expected_fv[0].first);
        ASSERT_EQ(fv_out[0].second, expected_fv[0].second);
    }
    ASSERT_LE(consumer.peek(), 0) << "Queue not empty after backward-compatible Pop.";
}

TEST_F(MultiNotificationConsumerTest, PopsOverloadBackwardCompatibility) {
    std::vector<std::string> channels = {"BC_POPS_CH1", "BC_POPS_CH2"};
    
    std::string op1 = "SET_BC1", key1 = "BC_KeyPops1";
    std::vector<swss::FieldValueTuple> fv1 = {{"f_bc1", "v_bc1"}};
    
    std::string op2 = "SET_BC2", key2 = "BC_KeyPops2";
    std::vector<swss::FieldValueTuple> fv2 = {{"f_bc2", "v_bc2"}};

    swss::MultiNotificationConsumer consumer(&m_db, channels);
    publishMessage(channels[0], op1, key1, fv1);
    publishMessage(channels[1], op2, key2, fv2);
    usleep(100000);

    ASSERT_GT(consumer.peek(), 0) << "No data ready for backward-compatible Pops.";
    std::deque<swss::KeyOpFieldsValuesTuple> received_tuples;
    consumer.pops(received_tuples); // Call the overload

    ASSERT_EQ(received_tuples.size(), 2);
    
    bool found_msg1 = false;
    bool found_msg2 = false;

    for (const auto& tuple : received_tuples) {
        std::string current_key = kfvKey(tuple); // Using common/table.h macros
        std::string current_op = kfvOp(tuple);
        std::vector<swss::FieldValueTuple> current_fv = kfvFieldsValues(tuple);

        if (current_key == key1 && current_op == op1) {
            found_msg1 = true;
            ASSERT_EQ(current_fv.size(), fv1.size());
            if (!fv1.empty()) {
                ASSERT_EQ(current_fv[0].first, fv1[0].first);
                ASSERT_EQ(current_fv[0].second, fv1[0].second);
            }
        } else if (current_key == key2 && current_op == op2) {
            found_msg2 = true;
            ASSERT_EQ(current_fv.size(), fv2.size());
            if (!fv2.empty()) {
                ASSERT_EQ(current_fv[0].first, fv2[0].first);
                ASSERT_EQ(current_fv[0].second, fv2[0].second);
            }
        }
    }
    ASSERT_TRUE(found_msg1) << "Message 1 (key: " << key1 << ") not found in backward-compatible pops.";
    ASSERT_TRUE(found_msg2) << "Message 2 (key: " << key2 << ") not found in backward-compatible pops.";
    ASSERT_LE(consumer.peek(), 0) << "Queue not empty after backward-compatible Pops.";
}

TEST_F(MultiNotificationConsumerTest, PsubscribeBasic) {
    std::vector<std::string> patterns = {"PATT_BASIC:*"};
    std::string ch1 = "PATT_BASIC:CH1", ch2 = "PATT_BASIC:CH2";
    std::string op1 = "SET", key1 = "KeyPBasic1";
    std::string op2 = "DEL", key2 = "KeyPBasic2";
    
    swss::MultiNotificationConsumer consumer(&m_db, {}); // No exact channels initially
    consumer.psubscribe(patterns);
    usleep(100000); // Allow psubscribe to process

    publishMessage(ch1, op1, key1, {});
    publishMessage(ch2, op2, key2, {});
    usleep(100000);

    std::deque<swss::ChannelKeyOpFieldsValuesTuple> received_tuples;
    ASSERT_GT(consumer.peek(), 0);
    consumer.pops(received_tuples);
    ASSERT_EQ(received_tuples.size(), 2);

    bool found_ch1 = false, found_ch2 = false;
    for (const auto& tuple : received_tuples) {
        if (ckfvChannel(tuple) == ch1 && ckfvKey(tuple) == key1 && ckfvOp(tuple) == op1) found_ch1 = true;
        if (ckfvChannel(tuple) == ch2 && ckfvKey(tuple) == key2 && ckfvOp(tuple) == op2) found_ch2 = true;
    }
    ASSERT_TRUE(found_ch1);
    ASSERT_TRUE(found_ch2);
    received_tuples.clear();

    // Test with popWithChannel
    publishMessage(ch1, op1, "KeyPBasic1_PopWC", {});
    usleep(100000);
    ASSERT_GT(consumer.peek(), 0);
    auto pair_result = consumer.popWithChannel();
    ASSERT_EQ(pair_result.first, ch1); // Verifies actual channel name

    // Test with pop(channel, op, data, fv)
    publishMessage(ch2, op2, "KeyPBasic2_PopFVT", {});
    usleep(100000);
    ASSERT_GT(consumer.peek(), 0);
    std::string pop_ch, pop_op, pop_key;
    std::vector<swss::FieldValueTuple> pop_fv;
    consumer.pop(pop_ch, pop_op, pop_key, pop_fv);
    ASSERT_EQ(pop_ch, ch2);
    ASSERT_EQ(pop_op, op2);
    ASSERT_EQ(pop_key, "KeyPBasic2_PopFVT");
}

TEST_F(MultiNotificationConsumerTest, PunsubscribeBasic) {
    std::string pattern = "PATT_UNSUB:*";
    std::string channel_to_pub = "PATT_UNSUB:CH1";
    swss::MultiNotificationConsumer consumer(&m_db, {});
    consumer.psubscribe({pattern});
    usleep(100000);

    publishMessage(channel_to_pub, "OP1", "KeyUnsub1", {});
    usleep(100000);
    ASSERT_GT(consumer.peek(), 0);
    std::deque<swss::ChannelKeyOpFieldsValuesTuple> received_tuples;
    consumer.pops(received_tuples);
    ASSERT_EQ(received_tuples.size(), 1);
    ASSERT_EQ(ckfvChannel(received_tuples.front()), channel_to_pub);
    received_tuples.clear();

    consumer.punsubscribe({pattern});
    usleep(100000);

    publishMessage(channel_to_pub, "OP2", "KeyUnsub2", {});
    usleep(100000);
    ASSERT_LE(consumer.peek(), 0); // Should not receive
    consumer.pops(received_tuples);
    ASSERT_TRUE(received_tuples.empty());
}

TEST_F(MultiNotificationConsumerTest, MixedSubscribeAndPsubscribe) {
    std::string exact_ch = "EXACT_CH_MIX";
    std::string pattern = "MIXED_PATT:*";
    std::string patt_ch1 = "MIXED_PATT:CH1";
    std::string patt_ch2 = "MIXED_PATT:CH2"; // Also matches exact_ch if exact_ch = "MIXED_PATT:EXACT"

    // Scenario 1: Exact channel does not match pattern
    swss::MultiNotificationConsumer consumer(&m_db, {exact_ch});
    consumer.psubscribe({pattern});
    usleep(100000);

    publishMessage(exact_ch, "OP_EXACT", "KEY_EXACT", {});
    publishMessage(patt_ch1, "OP_PATT1", "KEY_PATT1", {});
    publishMessage(patt_ch2, "OP_PATT2", "KEY_PATT2", {});
    usleep(100000);

    std::deque<swss::ChannelKeyOpFieldsValuesTuple> received_tuples;
    ASSERT_GT(consumer.peek(), 0);
    consumer.pops(received_tuples);
    ASSERT_EQ(received_tuples.size(), 3);
    // Verify contents, order not guaranteed
    bool found_exact=false, found_patt1=false, found_patt2=false;
    for(const auto& t : received_tuples) {
        if(ckfvChannel(t) == exact_ch && ckfvKey(t) == "KEY_EXACT") found_exact=true;
        if(ckfvChannel(t) == patt_ch1 && ckfvKey(t) == "KEY_PATT1") found_patt1=true;
        if(ckfvChannel(t) == patt_ch2 && ckfvKey(t) == "KEY_PATT2") found_patt2=true;
    }
    ASSERT_TRUE(found_exact && found_patt1 && found_patt2);
    received_tuples.clear();

    // Scenario 2: Exact channel ALSO matches pattern
    std::string exact_ch_matches_pattern = "MIXED_PATT:EXACT";
    swss::MultiNotificationConsumer consumer2(&m_db, {exact_ch_matches_pattern});
    consumer2.psubscribe({pattern}); // Same pattern "MIXED_PATT:*"
    usleep(100000);
    
    publishMessage(exact_ch_matches_pattern, "OP_MATCH", "KEY_MATCH", {});
    publishMessage(patt_ch1, "OP_PATT_OTHER", "KEY_PATT_OTHER", {}); // Another pattern match
    usleep(100000);

    ASSERT_GT(consumer2.peek(), 0);
    consumer2.pops(received_tuples);
    // Redis should only deliver one copy of the message for exact_ch_matches_pattern
    // Our processReply should handle this correctly.
    ASSERT_EQ(received_tuples.size(), 2); 
    bool found_match=false, found_patt_other=false;
    for(const auto& t : received_tuples) {
        if(ckfvChannel(t) == exact_ch_matches_pattern && ckfvKey(t) == "KEY_MATCH") found_match=true;
        if(ckfvChannel(t) == patt_ch1 && ckfvKey(t) == "KEY_PATT_OTHER") found_patt_other=true;
    }
    ASSERT_TRUE(found_match && found_patt_other);
}

TEST_F(MultiNotificationConsumerTest, UnsubscribeAllWithPsubscriptionsActive) {
    std::vector<std::string> exact_channels = {"EXACT_CH1_UA", "EXACT_CH2_UA"};
    std::string pattern = "PATT_ALL_A:*";
    std::string patt_ch3 = "PATT_ALL_A:CH3";
    
    swss::MultiNotificationConsumer consumer(&m_db, exact_channels);
    consumer.psubscribe({pattern});
    usleep(100000);

    publishMessage(exact_channels[0], "OP1", "KEY1", {});
    publishMessage(patt_ch3, "OP_PATT", "KEY_PATT", {});
    usleep(100000);
    std::deque<swss::ChannelKeyOpFieldsValuesTuple> deq;
    if(consumer.peek()>0) consumer.pops(deq);
    ASSERT_EQ(deq.size(), 2); // Exact + Pattern
    deq.clear();

    consumer.unsubscribeAll();
    usleep(100000);

    publishMessage(exact_channels[0], "OP_NEW", "KEY_NEW_EXACT", {}); // Should NOT be received
    publishMessage(patt_ch3, "OP_NEW", "KEY_NEW_PATT3", {});       // Should BE received
    std::string patt_ch4 = "PATT_ALL_A:CH4";
    publishMessage(patt_ch4, "OP_NEW", "KEY_NEW_PATT4", {});       // Should BE received
    usleep(100000);

    if(consumer.peek()>0) consumer.pops(deq);
    ASSERT_EQ(deq.size(), 2);
    bool found_patt3=false, found_patt4=false;
    for(const auto& t : deq) {
        if(ckfvChannel(t) == patt_ch3) found_patt3=true;
        if(ckfvChannel(t) == patt_ch4) found_patt4=true;
        ASSERT_NE(ckfvChannel(t), exact_channels[0]); // Ensure no exact channel msg
    }
    ASSERT_TRUE(found_patt3 && found_patt4);
}

TEST_F(MultiNotificationConsumerTest, PunsubscribeAllWithSubscriptionsActive) {
    std::string exact_ch = "EXACT_CH_B1_PUA";
    std::vector<std::string> patterns = {"PATT_ALL_B:*", "PATT_ALL_C:*"};
    std::string patt_b_ch = "PATT_ALL_B:CH1";
    std::string patt_c_ch = "PATT_ALL_C:CH2";

    swss::MultiNotificationConsumer consumer(&m_db, {exact_ch});
    consumer.psubscribe(patterns);
    usleep(100000);

    publishMessage(exact_ch, "OP_EX", "KEY_EX", {});
    publishMessage(patt_b_ch, "OP_PB", "KEY_PB", {});
    usleep(100000);
    std::deque<swss::ChannelKeyOpFieldsValuesTuple> deq;
    if(consumer.peek()>0) consumer.pops(deq);
    ASSERT_EQ(deq.size(), 2);
    deq.clear();

    consumer.punsubscribeAll();
    usleep(100000);

    publishMessage(exact_ch, "OP_EX_NEW", "KEY_EX_NEW", {});     // Should BE received
    publishMessage(patt_b_ch, "OP_PB_NEW", "KEY_PB_NEW", {});   // Should NOT be received
    publishMessage(patt_c_ch, "OP_PC_NEW", "KEY_PC_NEW", {});   // Should NOT be received (PATT_ALL_C was also punsubscribed)
    usleep(100000);

    if(consumer.peek()>0) consumer.pops(deq);
    ASSERT_EQ(deq.size(), 1);
    ASSERT_EQ(ckfvChannel(deq.front()), exact_ch);
    ASSERT_EQ(ckfvKey(deq.front()), "KEY_EX_NEW");
}

TEST_F(MultiNotificationConsumerTest, UnsubscribeAllAndPunsubscribeAll) {
    std::string exact_ch = "EXACT_CH_C1_UAPA";
    std::string pattern = "PATT_ALL_D:*";
    std::string patt_ch = "PATT_ALL_D:CH1";

    swss::MultiNotificationConsumer consumer(&m_db, {exact_ch});
    consumer.psubscribe({pattern});
    usleep(100000);

    publishMessage(exact_ch, "OP_EX", "KEY_EX", {});
    publishMessage(patt_ch, "OP_PD", "KEY_PD", {});
    usleep(100000);
    std::deque<swss::ChannelKeyOpFieldsValuesTuple> deq;
    if(consumer.peek()>0) consumer.pops(deq);
    ASSERT_EQ(deq.size(), 2);
    deq.clear();

    consumer.unsubscribeAll();
    consumer.punsubscribeAll();
    usleep(100000);

    publishMessage(exact_ch, "OP_EX_NEW", "KEY_EX_NEW", {});
    publishMessage(patt_ch, "OP_PD_NEW", "KEY_PD_NEW", {});
    usleep(100000);

    ASSERT_LE(consumer.peek(), 0);
    consumer.pops(deq);
    ASSERT_TRUE(deq.empty());
}

TEST_F(MultiNotificationConsumerTest, UnsubscribeNonAffectingPsubscribe) {
    std::string exact_ch = "EXACT_UNAFF";
    std::string pattern = "PATT_UNAFF:*";
    std::string patt_ch = "PATT_UNAFF:CH1";

    swss::MultiNotificationConsumer consumer(&m_db, {exact_ch});
    consumer.psubscribe({pattern});
    usleep(100000);

    publishMessage(exact_ch, "OP_EX", "KEY_EX", {});
    publishMessage(patt_ch, "OP_PATT", "KEY_PATT", {});
    usleep(100000);
    std::deque<swss::ChannelKeyOpFieldsValuesTuple> deq;
    if(consumer.peek()>0) consumer.pops(deq);
    ASSERT_EQ(deq.size(), 2);
    deq.clear();

    consumer.unsubscribe({exact_ch});
    usleep(100000);

    publishMessage(exact_ch, "OP_EX_NEW", "KEY_EX_NEW", {});     // Should NOT be received
    publishMessage(patt_ch, "OP_PATT_NEW", "KEY_PATT_NEW", {}); // Should BE received
    usleep(100000);

    if(consumer.peek()>0) consumer.pops(deq);
    ASSERT_EQ(deq.size(), 1);
    ASSERT_EQ(ckfvChannel(deq.front()), patt_ch);
    ASSERT_EQ(ckfvKey(deq.front()), "KEY_PATT_NEW");
}

TEST_F(MultiNotificationConsumerTest, PunsubscribeNonAffectingSubscribe) {
    std::string exact_ch = "EXACT_UNAFF2";
    std::string pattern = "PATT_UNAFF2:*";
    std::string patt_ch = "PATT_UNAFF2:CH1";

    swss::MultiNotificationConsumer consumer(&m_db, {exact_ch});
    consumer.psubscribe({pattern});
    usleep(100000);

    publishMessage(exact_ch, "OP_EX", "KEY_EX", {});
    publishMessage(patt_ch, "OP_PATT", "KEY_PATT", {});
    usleep(100000);
    std::deque<swss::ChannelKeyOpFieldsValuesTuple> deq;
    if(consumer.peek()>0) consumer.pops(deq);
    ASSERT_EQ(deq.size(), 2);
    deq.clear();

    consumer.punsubscribe({pattern});
    usleep(100000);

    publishMessage(exact_ch, "OP_EX_NEW", "KEY_EX_NEW", {});     // Should BE received
    publishMessage(patt_ch, "OP_PATT_NEW", "KEY_PATT_NEW", {}); // Should NOT be received
    usleep(100000);

    if(consumer.peek()>0) consumer.pops(deq);
    ASSERT_EQ(deq.size(), 1);
    ASSERT_EQ(ckfvChannel(deq.front()), exact_ch);
    ASSERT_EQ(ckfvKey(deq.front()), "KEY_EX_NEW");
}

TEST_F(MultiNotificationConsumerTest, UnsubscribeAllNoOp) {
    swss::MultiNotificationConsumer consumer(&m_db, {}); // No initial subscriptions
    ASSERT_NO_THROW(consumer.unsubscribeAll()); // Should be a graceful no-op
    usleep(50000); // Short delay
    // Verify no internal state change that might cause issues
    publishMessage("ANY_CH_UA", "OP", "KEY", {});
    usleep(50000);
    ASSERT_LE(consumer.peek(), 0);
}

TEST_F(MultiNotificationConsumerTest, PunsubscribeAllNoOp) {
    swss::MultiNotificationConsumer consumer(&m_db, {}); // No initial psubscriptions
    ASSERT_NO_THROW(consumer.punsubscribeAll()); // Should be a graceful no-op
    usleep(50000);
    publishMessage("ANY_CH_PUA", "OP", "KEY", {});
    usleep(50000);
    ASSERT_LE(consumer.peek(), 0);
}

