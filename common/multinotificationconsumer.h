#ifndef __MULTINOTIFICATIONCONSUMER__
#define __MULTINOTIFICATIONCONSUMER__

#include <string>
#include <vector>
#include <queue>
#include <deque> // Required for KeyOpFieldsValuesTuple
#include <utility> // For std::pair
#include <stdexcept> // For std::runtime_error

#include <hiredis/hiredis.h> // Required for redisReply

#include "dbconnector.h"
#include "json.h"
#include "logger.h"
#include "redisreply.h"
#include "selectable.h"
#include "table.h" // Required for FieldValueTuple and KeyOpFieldsValuesTuple

namespace swss {

// Represents a fully processed message: channel name, key, operation, and field-value pairs
using ChannelKeyOpFieldsValuesTuple = std::tuple<std::string, std::string, std::string, std::vector<FieldValueTuple>>;

static constexpr size_t DEFAULT_MULTI_NC_POP_BATCH_SIZE = 2048;

class MultiNotificationConsumer : public Selectable
{
public:
    MultiNotificationConsumer(swss::DBConnector *db, const std::vector<std::string> &channels, int pri = 100, size_t popBatchSize = DEFAULT_MULTI_NC_POP_BATCH_SIZE);

    // Pop one or multiple data from the internal queue which fed from redis socket
    // Note:
    //    Ensure data ready before popping, either by select or peek
    void pop(std::string &channel, std::string &op, std::string &data, std::vector<FieldValueTuple> &values);
    void pops(std::deque<ChannelKeyOpFieldsValuesTuple> &vckco);

    // Overload for backward compatibility (processes JSON, no channel output)
    void pop(std::string &op, std::string &data, std::vector<FieldValueTuple> &values);

    // Overload for backward compatibility (processes JSON, no channel output)
    void pops(std::deque<swss::KeyOpFieldsValuesTuple> &vkco);

    // Pops raw message payload string from a channel.
    // Throws std::runtime_error if queue is empty.
    std::string popPayload();

    // Pops raw (channel_name, message_payload) pair.
    // Throws std::runtime_error if queue is empty.
    std::pair<std::string, std::string> popWithChannel();

    // Check the internal queue which fed from redis socket for data ready
    // Returns:
    //     1 - data immediately available inside internal queue, may be just fed from redis socket
    //     0 - no data both in internal queue or redis socket
    //    -1 - error during peeking redis socket
    int peek();

    // Unsubscribes from the specified channels.
    void unsubscribe(const std::vector<std::string>& channels_to_unsubscribe);

    // Subscribes to the given Redis channel patterns.
    void psubscribe(const std::vector<std::string>& patterns);

    // Unsubscribes from the specified Redis channel patterns.
    void punsubscribe(const std::vector<std::string>& patterns);

    // Unsubscribes from all currently subscribed exact channels.
    void unsubscribeAll();

    // Unsubscribes from all currently subscribed channel patterns.
    void punsubscribeAll();

    ~MultiNotificationConsumer() override;

    int getFd() override;
    uint64_t readData() override;
    bool hasData() override;
    bool hasCachedData() override;
    const size_t POP_BATCH_SIZE;

private:
    MultiNotificationConsumer(const MultiNotificationConsumer &other) = delete;
    MultiNotificationConsumer& operator = (const MultiNotificationConsumer &other) = delete;

    void processReply(redisReply *reply);
    void subscribe();

    swss::DBConnector *m_db; // Unused, but kept for consistency with NotificationConsumer
    swss::DBConnector *m_subscribe;
    std::vector<std::string> m_channels;
    std::vector<std::string> m_patterns; // Stores active pattern subscriptions
    std::queue<std::pair<std::string, std::string>> m_queue; // Stores <channel_name, message_payload>
};

} // namespace swss

#endif // __MULTINOTIFICATIONCONSUMER__
