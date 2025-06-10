#include <gtest/gtest.h>
#include "common/ipprefix.h"

#include <iostream>
#include <thread>

#include <unistd.h>

#include "common/notificationconsumer.h"
#include "common/notificationproducer.h"
#include "common/selectableevent.h"
#include "common/select.h"
#include "common/logger.h"
#include "common/table.h"
#include "common/filterednotificationconsumer.h"

std::shared_ptr<std::thread> notification_thread;

const int messages = 5000;

void ntf_thread(swss::NotificationConsumer& nc)
{
    SWSS_LOG_ENTER();

    swss::Select s;

    s.addSelectable(&nc);

    int collected = 0;

    while (collected < messages)
    {
        swss::Selectable *sel;

        int result = s.select(&sel);

        if (result == swss::Select::OBJECT)
        {
            swss::KeyOpFieldsValuesTuple kco;

            std::string op;
            std::string data;
            std::vector<swss::FieldValueTuple> values;

            nc.pop(op, data, values);

            SWSS_LOG_INFO("notification: op = %s, data = %s", op.c_str(), data.c_str());
            EXPECT_EQ(op, "ntf");
            int i = stoi(data);
            EXPECT_EQ(i, collected + 1);

            collected++;
        }
    }
    EXPECT_EQ(collected, messages);
}

TEST(Notifications, test)
{
    SWSS_LOG_ENTER();

    swss::DBConnector dbNtf("ASIC_DB", 0, true);
    swss::NotificationConsumer nc(&dbNtf, "NOTIFICATIONS");
    notification_thread = std::make_shared<std::thread>(std::thread(ntf_thread, std::ref(nc)));

    swss::NotificationProducer notifications(&dbNtf, "NOTIFICATIONS");

    std::vector<swss::FieldValueTuple> entry;

    for(int i = 0; i < messages; i++)
    {
        std::string s = std::to_string(i+1);

        auto sentClients = notifications.send("ntf", s, entry);
        EXPECT_EQ(sentClients, 1);
    }

    notification_thread->join();
}

TEST(Notifications, pops)
{
    SWSS_LOG_ENTER();

    swss::DBConnector dbNtf("ASIC_DB", 0, true);
    swss::NotificationConsumer nc(&dbNtf, "NOTIFICATIONS", 100, (size_t)messages);
    swss::NotificationProducer notifications(&dbNtf, "NOTIFICATIONS");

    std::vector<swss::FieldValueTuple> entry;
    for(int i = 0; i < messages; i++)
    {
        auto s = std::to_string(i+1);
        auto sentClients = notifications.send("ntf", s, entry);
        EXPECT_EQ(sentClients, 1);
    }

    // Pop all the notifications
    swss::Select s;
    s.addSelectable(&nc);
    swss::Selectable *sel;

    int result = s.select(&sel);

    EXPECT_EQ(result, swss::Select::OBJECT);

    std::deque<swss::KeyOpFieldsValuesTuple> vkco;
    nc.pops(vkco);
    EXPECT_EQ(vkco.size(), (size_t)messages);

    for (size_t collected = 0; collected < vkco.size(); collected++)
    {
        auto data = kfvKey(vkco[collected]);
        auto op = kfvOp(vkco[collected]);

        EXPECT_EQ(op, "ntf");
        int i = stoi(data);
        EXPECT_EQ((size_t)i, collected + 1);
    }

    // Peek and get nothing more
    int rc = nc.peek();
    EXPECT_EQ(rc, 0);
}

TEST(FilteredNotifications, NoTopicsSubscribed)
{
    SWSS_LOG_ENTER();
    swss::DBConnector dbNtf("ASIC_DB", 0, true);
    swss::NotificationProducer producer(&dbNtf, "FN_CHAN_NT");
    swss::FilteredNotificationConsumer consumer(&dbNtf, "FN_CHAN_NT");

    std::vector<swss::FieldValueTuple> fvA, fvB;
    fvA.push_back(swss::FieldValueTuple(swss::JSON_FIELD_ENTITY_ID, "TOPIC_A"));
    fvA.push_back(swss::FieldValueTuple("value", "valA"));

    fvB.push_back(swss::FieldValueTuple("value", "valB_no_entity_id")); // No _entity_id

    producer.send("SET", "KEY_A_NT", fvA);
    producer.send("SET", "KEY_B_NT", fvB);

    swss::Select s;
    s.addSelectable(&consumer);
    int received_count = 0;
    std::string op, data;
    std::vector<swss::FieldValueTuple> values;
    swss::Selectable *sel = nullptr;

    // Expect to receive both messages due to pass-through logic when no topics subscribed
    ASSERT_EQ(s.select(&sel, 1000), swss::Select::OBJECT) << "Expected message KEY_A_NT";
    consumer.pop(op, data, values);
    ASSERT_EQ(data, "KEY_A_NT");
    received_count++;

    ASSERT_EQ(s.select(&sel, 1000), swss::Select::OBJECT) << "Expected message KEY_B_NT";
    consumer.pop(op, data, values);
    ASSERT_EQ(data, "KEY_B_NT");
    received_count++;

    ASSERT_EQ(received_count, 2);

    int result = s.select(&sel, 100);
    ASSERT_NE(result, swss::Select::OBJECT);
    bool pop_threw = false;
    try {
        consumer.pop(op, data, values);
    } catch (const std::runtime_error& e) {
        pop_threw = true;
    }
    ASSERT_TRUE(pop_threw);
}

TEST(FilteredNotifications, Unsubscribe)
{
    SWSS_LOG_ENTER();
    swss::DBConnector dbNtf("ASIC_DB", 0, true);
    swss::NotificationProducer producer(&dbNtf, "FN_CHAN_US");
    swss::FilteredNotificationConsumer consumer(&dbNtf, "FN_CHAN_US");

    consumer.subscribeTopic("TOPIC_A_US");
    consumer.subscribeTopic("TOPIC_B_US");

    std::vector<swss::FieldValueTuple> fvA, fvB, fvC;
    fvA.push_back(swss::FieldValueTuple(swss::JSON_FIELD_ENTITY_ID, "TOPIC_A_US"));
    fvB.push_back(swss::FieldValueTuple(swss::JSON_FIELD_ENTITY_ID, "TOPIC_B_US"));
    fvC.push_back(swss::FieldValueTuple(swss::JSON_FIELD_ENTITY_ID, "TOPIC_C_US")); // Not subscribed

    producer.send("OP", "KEY_A1_US", fvA);
    producer.send("OP", "KEY_B1_US", fvB);
    producer.send("OP", "KEY_C1_US", fvC); // Should be filtered

    swss::Select s;
    s.addSelectable(&consumer);
    std::string op, data;
    std::vector<swss::FieldValueTuple> values;
    swss::Selectable *sel = nullptr;

    ASSERT_EQ(s.select(&sel, 1000), swss::Select::OBJECT); consumer.pop(op, data, values);
    ASSERT_TRUE(data == "KEY_A1_US" || data == "KEY_B1_US") << "Data: " << data;
    ASSERT_EQ(s.select(&sel, 1000), swss::Select::OBJECT); consumer.pop(op, data, values);
    ASSERT_TRUE(data == "KEY_A1_US" || data == "KEY_B1_US") << "Data: " << data;

    consumer.unsubscribeTopic("TOPIC_A_US");
    producer.send("OP", "KEY_A2_US", fvA); // Should be filtered out now
    producer.send("OP", "KEY_B2_US", fvB); // Should be received
    producer.send("OP", "KEY_C2_US", fvC); // Still filtered

    ASSERT_EQ(s.select(&sel, 1000), swss::Select::OBJECT);
    consumer.pop(op, data, values);
    ASSERT_EQ(data, "KEY_B2_US");

    int result = s.select(&sel, 100);
    ASSERT_NE(result, swss::Select::OBJECT);
    bool pop_threw = false;
    try {
        consumer.pop(op, data, values);
    } catch (const std::runtime_error& e) {
        pop_threw = true;
    }
    ASSERT_TRUE(pop_threw);
}

TEST(FilteredNotifications, PopsMethod)
{
    SWSS_LOG_ENTER();
    swss::DBConnector dbNtf("ASIC_DB", 0, true);
    swss::NotificationProducer producer(&dbNtf, "FN_CHAN_POPS");
    swss::FilteredNotificationConsumer consumer(&dbNtf, "FN_CHAN_POPS");

    consumer.subscribeTopic("EVEN_TOPIC");

    for (int i = 0; i < 10; ++i) {
        std::vector<swss::FieldValueTuple> fv;
        std::string topic = (i % 2 == 0) ? "EVEN_TOPIC" : "ODD_TOPIC";
        fv.push_back(swss::FieldValueTuple(swss::JSON_FIELD_ENTITY_ID, topic));
        fv.push_back(swss::FieldValueTuple("val", std::to_string(i)));
        producer.send("SET", "KEY_" + std::to_string(i), fv);
    }

    swss::Select s;
    s.addSelectable(&consumer);
    swss::Selectable *sel = nullptr;

    int result = s.select(&sel, 1000); // Wait for messages to arrive
    ASSERT_EQ(result, swss::Select::OBJECT);

    std::deque<swss::KeyOpFieldsValuesTuple> kco_deque;
    consumer.pops(kco_deque); // Pop all available filtered messages

    ASSERT_EQ(kco_deque.size(), 5) << "Should only receive 5 messages for EVEN_TOPIC";
    for(const auto& kco : kco_deque) {
        bool found_entity_id = false;
        for(const auto& fv_pair_pops : kfvFieldsValues(kco)) {
            if (fvField(fv_pair_pops) == swss::JSON_FIELD_ENTITY_ID) {
                ASSERT_EQ(fvValue(fv_pair_pops), "EVEN_TOPIC");
                found_entity_id = true;
                break;
            }
        }
        ASSERT_TRUE(found_entity_id);
    }

    // Ensure no more messages are in the filtered queue
    kco_deque.clear();
    consumer.pops(kco_deque);
    ASSERT_TRUE(kco_deque.empty()) << "Filtered queue should be empty after pops";

    // Ensure no more messages on the channel that would match
    result = s.select(&sel, 100);
    ASSERT_NE(result, swss::Select::OBJECT);
}

TEST(FilteredNotifications, SpecificTopics)
{
    SWSS_LOG_ENTER();
    swss::DBConnector dbNtf("ASIC_DB", 0, true);
    // Unique channel per test to avoid interference
    swss::NotificationProducer producer(&dbNtf, "FN_CHAN_ST");
    swss::FilteredNotificationConsumer consumer(&dbNtf, "FN_CHAN_ST");

    consumer.subscribeTopic("TOPIC_A");
    consumer.subscribeTopic("TOPIC_C");

    std::vector<swss::FieldValueTuple> fvA, fvB, fvC;
    fvA.push_back(swss::FieldValueTuple(swss::JSON_FIELD_ENTITY_ID, "TOPIC_A"));
    fvA.push_back(swss::FieldValueTuple("value", "valA"));

    fvB.push_back(swss::FieldValueTuple(swss::JSON_FIELD_ENTITY_ID, "TOPIC_B")); // This topic is not subscribed
    fvB.push_back(swss::FieldValueTuple("value", "valB"));

    fvC.push_back(swss::FieldValueTuple(swss::JSON_FIELD_ENTITY_ID, "TOPIC_C"));
    fvC.push_back(swss::FieldValueTuple("value", "valC"));

    producer.send("SET", "KEY_A", fvA);
    producer.send("SET", "KEY_B", fvB); // Should be filtered out
    producer.send("SET", "KEY_C", fvC);

    swss::Select s;
    s.addSelectable(&consumer);

    int received_count = 0;
    std::string op, data;
    std::vector<swss::FieldValueTuple> values;
    swss::Selectable *sel = nullptr;

    // Expecting KEY_A
    ASSERT_EQ(s.select(&sel, 1000), swss::Select::OBJECT) << "Expected message KEY_A";
    ASSERT_EQ(sel, &consumer);
    consumer.pop(op, data, values);
    ASSERT_EQ(data, "KEY_A");
    received_count++;

    // Expecting KEY_C
    ASSERT_EQ(s.select(&sel, 1000), swss::Select::OBJECT) << "Expected message KEY_C";
    ASSERT_EQ(sel, &consumer);
    consumer.pop(op, data, values);
    ASSERT_EQ(data, "KEY_C");
    received_count++;

    ASSERT_EQ(received_count, 2);

    // Try to pop again, should timeout or throw if no more matching messages
    int result = s.select(&sel, 100); // Short timeout
    ASSERT_NE(result, swss::Select::OBJECT) << "Expected no more messages";

    bool pop_threw = false;
    try {
        consumer.pop(op, data, values);
    } catch (const std::runtime_error& e) {
        pop_threw = true;
    }
    ASSERT_TRUE(pop_threw) << "Pop should have thrown on an empty filtered queue";
}

TEST(Notifications, peek)
{
    SWSS_LOG_ENTER();

    swss::DBConnector dbNtf("ASIC_DB", 0, true);
    swss::NotificationConsumer nc(&dbNtf, "NOTIFICATIONS", 100, (size_t)10);
    swss::NotificationProducer notifications(&dbNtf, "NOTIFICATIONS");

    std::vector<swss::FieldValueTuple> entry;
    for(int i = 0; i < messages; i++)
    {
        auto s = std::to_string(i+1);
        auto sentClients = notifications.send("ntf", s, entry);
        EXPECT_EQ(sentClients, 1);
    }

    // Pop all the notifications
    std::deque<swss::KeyOpFieldsValuesTuple> vkco;
    size_t popped = 0;
    size_t npop = 10000;
    int collected = 0;
    while(nc.peek() > 0 && popped < npop)
    {
        nc.pops(vkco);
        popped += vkco.size();

        for (auto& kco : vkco)
        {
            collected++;
            auto data = kfvKey(kco);
            auto op = kfvOp(kco);

            EXPECT_EQ(op, "ntf");
            int i = stoi(data);
            EXPECT_EQ(i, collected);
        }
    }
    EXPECT_EQ(popped, (size_t)messages);

    // Peek and get nothing more
    int rc = nc.peek();
    EXPECT_EQ(rc, 0);
}

TEST(Notifications, pipelineProducer)
{
    SWSS_LOG_ENTER();

    swss::DBConnector dbNtf("ASIC_DB", 0, true);
    swss::RedisPipeline pipeline{&dbNtf};
    swss::NotificationConsumer nc(&dbNtf, "NOTIFICATIONS", 100, (size_t)10);
    const bool buffered = true;
    swss::NotificationProducer notifications(&pipeline, "NOTIFICATIONS", buffered);

    std::vector<swss::FieldValueTuple> entry;
    for(int i = 0; i < messages; i++)
    {
        auto s = std::to_string(i+1);
        auto sentClients = notifications.send("ntf", s, entry);
        // In buffered mode we get -1 in return
        EXPECT_EQ(sentClients, -1);
    }

    // Flush the pipeline
    pipeline.flush();

    // Pop all the notifications
    std::deque<swss::KeyOpFieldsValuesTuple> vkco;
    size_t popped = 0;
    size_t npop = 10000;
    int collected = 0;
    while(nc.peek() > 0 && popped < npop)
    {
        nc.pops(vkco);
        popped += vkco.size();

        for (auto& kco : vkco)
        {
            collected++;
            auto data = kfvKey(kco);
            auto op = kfvOp(kco);

            EXPECT_EQ(op, "ntf");
            int i = stoi(data);
            EXPECT_EQ(i, collected);
        }
    }
    EXPECT_EQ(popped, (size_t)messages);

    // Peek and get nothing more
    int rc = nc.peek();
    EXPECT_EQ(rc, 0);
}

