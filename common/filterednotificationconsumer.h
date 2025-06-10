#pragma once

#include <string>
#include <vector>
#include <set>
#include <deque>
#include "common/notificationconsumer.h" // For KeyOpFieldsValuesTuple, FieldValueTuple
#include "common/selectable.h"         // To be a selectable object itself

namespace swss {

// Define the key for entity ID here for clarity and use.
const std::string JSON_FIELD_ENTITY_ID = "_entity_id";

class FilteredNotificationConsumer : public Selectable {
public:
    FilteredNotificationConsumer(swss::DBConnector *db, const std::string &channel, int pri = 0, size_t popBatchSize = NotificationConsumer::DEFAULT_POP_BATCH_SIZE);
    ~FilteredNotificationConsumer();

    // Disable copy and assignment
    FilteredNotificationConsumer(const FilteredNotificationConsumer&) = delete;
    FilteredNotificationConsumer& operator=(const FilteredNotificationConsumer&) = delete;

    void subscribeTopic(const std::string& entity_topic);
    void unsubscribeTopic(const std::string& entity_topic);
    bool isTopicSubscribed(const std::string& entity_topic) const;

    void pop(std::string &op, std::string &data, std::vector<FieldValueTuple> &values);
    void pops(std::deque<KeyOpFieldsValuesTuple> &vkco);

    int getFd() override;
    uint64_t readData() override;
    bool hasData() override;
    bool hasCachedData() override;

    int peek();

private:
    NotificationConsumer m_notification_consumer;
    std::set<std::string> m_subscribed_topics;
    std::deque<KeyOpFieldsValuesTuple> m_filtered_data_queue;

    void filterAndQueue();
};

} // namespace swss
