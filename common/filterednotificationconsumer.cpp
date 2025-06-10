#include "filterednotificationconsumer.h"
#include "common/logger.h" // For SWSS_LOG_DEBUG

namespace swss {

FilteredNotificationConsumer::FilteredNotificationConsumer(swss::DBConnector *db, const std::string &channel, int pri, size_t popBatchSize)
    : Selectable(pri),
      m_notification_consumer(db, channel, pri, popBatchSize) {
}

FilteredNotificationConsumer::~FilteredNotificationConsumer() {
}

void FilteredNotificationConsumer::subscribeTopic(const std::string& entity_topic) {
    m_subscribed_topics.insert(entity_topic);
}

void FilteredNotificationConsumer::unsubscribeTopic(const std::string& entity_topic) {
    m_subscribed_topics.erase(entity_topic);
}

bool FilteredNotificationConsumer::isTopicSubscribed(const std::string& entity_topic) const {
    return m_subscribed_topics.count(entity_topic) > 0;
}

void FilteredNotificationConsumer::filterAndQueue() {
    std::deque<KeyOpFieldsValuesTuple> raw_data;
    m_notification_consumer.pops(raw_data); // Pops all available from underlying consumer

    for (const auto& item : raw_data) {
        // const std::string& key = kfvKey(item); // For debugging if needed
        // const std::string& op = kfvOp(item);   // For debugging if needed
        const std::vector<FieldValueTuple>& fields = kfvFieldsValues(item);

        bool matched = m_subscribed_topics.empty();
        if (!matched) {
            for (const auto& fv_pair : fields) {
                if (fvField(fv_pair) == JSON_FIELD_ENTITY_ID) {
                    if (m_subscribed_topics.count(fvValue(fv_pair))) {
                        matched = true;
                        break;
                    }
                }
            }
        }

        if (matched) {
            m_filtered_data_queue.push_back(item);
        } else {
            // SWSS_LOG_DEBUG("FilteredNotificationConsumer: Discarded message for key '%s', op '%s' due to topic mismatch.",
            // kfvKey(item).c_str(), kfvOp(item).c_str());
        }
    }
}

void FilteredNotificationConsumer::pop(std::string &op, std::string &data, std::vector<FieldValueTuple> &values) {
    if (m_filtered_data_queue.empty()) {
        if (m_notification_consumer.peek() > 0) { // Check if underlying has data
             filterAndQueue(); // Attempt to read and filter
        }
    }

    if (m_filtered_data_queue.empty()) {
        // SWSS_LOG_ERROR("FilteredNotificationConsumer: Queue is empty, can't pop."); // This might be too verbose for normal operation
        throw std::runtime_error("FilteredNotificationConsumer: Queue is empty, can't pop");
    }

    const KeyOpFieldsValuesTuple& item = m_filtered_data_queue.front();

    data = kfvKey(item);
    op = kfvOp(item);
    values = kfvFieldsValues(item);

    m_filtered_data_queue.pop_front();
}

void FilteredNotificationConsumer::pops(std::deque<KeyOpFieldsValuesTuple> &vkco) {
    vkco.clear();
    if (m_filtered_data_queue.empty()) {
        if (m_notification_consumer.peek() > 0) {
            filterAndQueue();
        }
    }
    // Efficiently move all currently available filtered items.
    std::swap(vkco, m_filtered_data_queue);
}

int FilteredNotificationConsumer::getFd() {
    return m_notification_consumer.getFd();
}

uint64_t FilteredNotificationConsumer::readData() {
    // This function is called by Select framework when m_notification_consumer.getFd() is ready.
    // We need to read from m_notification_consumer and put it into m_filtered_data_queue.
    filterAndQueue();
    // The return value for readData is not strictly defined for correctness by Select,
    // but returning non-zero could indicate an error if one occurred.
    // Here, we assume filterAndQueue handles its own errors or they are exceptional.
    return 0;
}

bool FilteredNotificationConsumer::hasData() {
    if (m_filtered_data_queue.empty()) {
        // If Selectable is used in a tight loop, repeated peeking might be costly.
        // However, for typical Select usage, hasData is called after Select indicates readability.
        // Peeking here ensures that if readData was called but no *matching* data was found,
        // hasData can re-trigger a filter attempt if new raw data arrived quickly.
        if (m_notification_consumer.peek() > 0) {
            filterAndQueue();
        }
    }
    return !m_filtered_data_queue.empty();
}

bool FilteredNotificationConsumer::hasCachedData() {
    if (m_filtered_data_queue.empty()) {
        if (m_notification_consumer.peek() > 0) {
            filterAndQueue();
        }
    }
    return m_filtered_data_queue.size() > 1;
}

int FilteredNotificationConsumer::peek() {
    if (m_filtered_data_queue.empty()) {
        if (m_notification_consumer.peek() > 0) { // Check underlying consumer
            filterAndQueue(); // Attempt to populate filtered queue
        }
    }
    return m_filtered_data_queue.empty() ? 0 : 1; // Return status of filtered queue
}

} // namespace swss
