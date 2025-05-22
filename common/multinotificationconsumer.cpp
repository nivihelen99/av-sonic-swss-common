#include "multinotificationconsumer.h"

#include <iostream> // For potential debugging, can be removed later
#include <algorithm> // For std::remove_if, std::find
#include "redisapi.h" // For peekRedisContext, etc.

#define NOTIFICATION_SUBSCRIBE_TIMEOUT (1000) // Same as NotificationConsumer
#define REDIS_PUB_SUB_MESSAGE_TYPE_INDEX (0)
#define REDIS_PUB_SUB_CHANNEL_INDEX (1)
#define REDIS_PUB_SUB_PAYLOAD_INDEX (2)
#define REDIS_PUB_SUB_MESSAGE_ELEMENTS (3) // "message", "channel", "payload"

namespace swss {

MultiNotificationConsumer::MultiNotificationConsumer(swss::DBConnector *db, const std::vector<std::string> &channels, int pri, size_t popBatchSize) :
    Selectable(pri),
    POP_BATCH_SIZE(popBatchSize),
    m_db(db), // Store the main DB connector
    m_subscribe(nullptr),
    m_channels(channels)
{
    SWSS_LOG_ENTER();

    if (m_channels.empty())
    {
        SWSS_LOG_ERROR("No channels provided for MultiNotificationConsumer.");
        throw std::invalid_argument("Channels list cannot be empty for MultiNotificationConsumer");
    }

    // Attempt to subscribe, with retry logic similar to NotificationConsumer
    while (true)
    {
        try
        {
            subscribe();
            break; // Success
        }
        catch (const std::exception& e)
        {
            SWSS_LOG_ERROR("Failed to subscribe to channels: %s. Retrying...", e.what());
            delete m_subscribe;
            m_subscribe = nullptr;
            // Consider adding a sleep here if retries are too frequent
            // std::this_thread::sleep_for(std::chrono::seconds(1)); 
        }
        catch (...)
        {
            SWSS_LOG_ERROR("Failed to subscribe to channels due to unknown exception. Retrying...");
            delete m_subscribe;
            m_subscribe = nullptr;
            // Consider adding a sleep here
        }
    }
}

MultiNotificationConsumer::~MultiNotificationConsumer()
{
    SWSS_LOG_ENTER();
    delete m_subscribe;
}

void MultiNotificationConsumer::subscribe()
{
    SWSS_LOG_ENTER();

    // Create new context to DB for subscriptions
    if (m_db->getContext()->connection_type == REDIS_CONN_TCP)
    {
        m_subscribe = new DBConnector(m_db->getDbId(),
                                      m_db->getContext()->tcp.host,
                                      m_db->getContext()->tcp.port,
                                      NOTIFICATION_SUBSCRIBE_TIMEOUT);
    }
    else
    {
        m_subscribe = new DBConnector(m_db->getDbId(),
                                      m_db->getContext()->unix_sock.path,
                                      NOTIFICATION_SUBSCRIBE_TIMEOUT);
    }

    std::string cmd = "SUBSCRIBE";
    for (const auto& channel : m_channels)
    {
        cmd += " " + channel;
    }

    RedisReply r(m_subscribe, cmd, REDIS_REPLY_ARRAY);

    // hiredis documentation states that the reply to SUBSCRIBE is an array where:
    // - The first element is "subscribe".
    // - The second element is the channel name.
    // - The third element is the number of channels currently subscribed to.
    // This pattern repeats for each channel in the SUBSCRIBE command.
    // We need to check the replies carefully.
    for (size_t i = 0; i < m_channels.size(); ++i)
    {
        redisReply *reply = r.getReply(); // Get the raw reply to inspect its structure for each subscription
        if (!reply || reply->type != REDIS_REPLY_ARRAY || reply->elements != 3) {
            SWSS_LOG_ERROR("Unexpected reply format during SUBSCRIBE for channel %s", m_channels[i].c_str());
            throw std::runtime_error("Failed to subscribe to channel " + m_channels[i]);
        }
        // reply->element[0] should be "subscribe"
        // reply->element[1] should be the channel name
        // reply->element[2] should be the count of subscriptions
        if (std::string(reply->element[0]->str) != "subscribe" || std::string(reply->element[1]->str) != m_channels[i]) {
            SWSS_LOG_ERROR("Unexpected content in SUBSCRIBE reply for channel %s", m_channels[i].c_str());
            throw std::runtime_error("Unexpected content in subscribe reply for " + m_channels[i]);
        }
        SWSS_LOG_INFO("Successfully subscribed to channel: %s", m_channels[i].c_str());
        if (i < m_channels.size() -1) { // If there are more channels, get next reply part
             r.next(); // Advance to the next part of the batched reply.
        }
    }
    SWSS_LOG_INFO("Successfully subscribed to all %zu channels.", m_channels.size());
}

int MultiNotificationConsumer::getFd()
{
    if (!m_subscribe || !m_subscribe->getContext())
    {
        SWSS_LOG_ERROR("Subscription DB connector or context is null in getFd.");
        // This might indicate an issue during construction or a premature call.
        // Depending on error handling strategy, could throw or return -1.
        // For now, mimic original behavior which might lead to a crash if not checked by caller.
        return -1; 
    }
    return m_subscribe->getContext()->fd;
}

uint64_t MultiNotificationConsumer::readData()
{
    SWSS_LOG_ENTER();
    if (!m_subscribe || !m_subscribe->getContext())
    {
        SWSS_LOG_ERROR("Subscription DB connector or context is null in readData.");
        throw std::runtime_error("Subscription DB connector is not initialized in readData");
    }

    redisReply *reply = nullptr;

    if (redisGetReply(m_subscribe->getContext(), reinterpret_cast<void**>(&reply)) != REDIS_OK)
    {
        std::string err_msg = "Failed to read redis reply";
        if (m_subscribe->getContext()->errstr) {
            err_msg += std::string(": ") + m_subscribe->getContext()->errstr;
        }
        SWSS_LOG_ERROR("%s", err_msg.c_str());
        throw std::runtime_error(err_msg);
    }
    else
    {
        if (reply == nullptr) { // Should not happen if REDIS_OK is returned
             SWSS_LOG_ERROR("redisGetReply returned REDIS_OK but reply is null");
             throw std::runtime_error("redisGetReply inconsistent state");
        }
        RedisReply r(reply); // r takes ownership of reply
        processReply(reply); // processReply does not take ownership
    }

    // Process any buffered replies
    reply = nullptr; // Reset for next use
    int status;
    do
    {
        // redisReaderGetReply is non-blocking if data is available in the reader buffer
        status = redisReaderGetReply(m_subscribe->getContext()->reader, reinterpret_cast<void**>(&reply));
        if (reply != nullptr && status == REDIS_OK)
        {
            RedisReply r(reply); // r takes ownership
            processReply(reply); // processReply does not take ownership
        }
        else if (status != REDIS_OK && status != REDIS_ERR_IO) // REDIS_ERR_IO means non-blocking would block
        {
             std::string err_msg = "Failed to read buffered redis reply";
             if (m_subscribe->getContext()->errstr) {
                 err_msg += std::string(": ") + m_subscribe->getContext()->errstr;
             }
             SWSS_LOG_ERROR("%s", err_msg.c_str());
             throw std::runtime_error(err_msg);
        }
    }
    while (reply != nullptr && status == REDIS_OK);


    return 0; // Return value seems unused in original, keeping consistent.
}

bool MultiNotificationConsumer::hasData()
{
    return !m_queue.empty();
}

bool MultiNotificationConsumer::hasCachedData()
{
    // This logic might need refinement. If "cached" means more than one item,
    // it's fine. If it means items peeked but not yet popped by the application,
    // this is also fine.
    return m_queue.size() > 1;
}

void MultiNotificationConsumer::processReply(redisReply *reply)
{
    SWSS_LOG_ENTER();

    if (reply == nullptr)
    {
        SWSS_LOG_WARN("processReply called with null reply.");
        return;
    }

    // Messages from PSUBSCRIBE/SUBSCRIBE come as an array:
    // 1. "message" (string)
    // 2. The originating channel (string)
    // 3. The actual message/payload (string)
    if (reply->type != REDIS_REPLY_ARRAY)
    {
        // Could be a control message, e.g. reply to SUBSCRIBE itself.
        // For now, we only care about "message" type.
        // SWSS_LOG_DEBUG("Reply is not ARRAY, type: %d. Ignoring.", reply->type);
        return;
    }

    if (reply->elements != REDIS_PUB_SUB_MESSAGE_ELEMENTS)
    {
        // This could also be a reply to the initial SUBSCRIBE/UNSUBSCRIBE command.
        // For actual messages, it must be 3 elements.
        // Check if it's a subscribe, psubscribe, or unsubscribe confirmation.
        if (reply->elements > 0 && reply->element[REDIS_PUB_SUB_MESSAGE_TYPE_INDEX]->type == REDIS_REPLY_STRING)
        {
            std::string type_str = reply->element[REDIS_PUB_SUB_MESSAGE_TYPE_INDEX]->str;
            if (type_str == "subscribe" || type_str == "psubscribe" || type_str == "unsubscribe")
            {
                SWSS_LOG_DEBUG("Received '%s' confirmation, not a message. Channel: %s. Ignoring.", 
                               type_str.c_str(), 
                               (reply->elements > 1 && reply->element[REDIS_PUB_SUB_CHANNEL_INDEX]->type == REDIS_REPLY_STRING && reply->element[REDIS_PUB_SUB_CHANNEL_INDEX]->str) ? reply->element[REDIS_PUB_SUB_CHANNEL_INDEX]->str : "N/A");
                return;
            }
        }

        SWSS_LOG_ERROR("Expected %d elements in redis PUB/SUB reply for a message, got: %zu. First element type: %d, value: %s",
                       REDIS_PUB_SUB_MESSAGE_ELEMENTS,
                       reply->elements,
                       reply->element[REDIS_PUB_SUB_MESSAGE_TYPE_INDEX]->type,
                       reply->element[REDIS_PUB_SUB_MESSAGE_TYPE_INDEX]->str);
        // Not throwing an error here to be more resilient to unexpected control messages.
        // Consumer should rely on hasData() and pop() to get valid messages.
        return;
    }
    
    if (reply->element[REDIS_PUB_SUB_MESSAGE_TYPE_INDEX]->type != REDIS_REPLY_STRING ||
        std::string(reply->element[REDIS_PUB_SUB_MESSAGE_TYPE_INDEX]->str) != "message")
    {
        SWSS_LOG_DEBUG("Reply is not of type 'message'. Type: '%s'. Ignoring.",
            reply->element[REDIS_PUB_SUB_MESSAGE_TYPE_INDEX]->str ? reply->element[REDIS_PUB_SUB_MESSAGE_TYPE_INDEX]->str : "null");
        return;
    }

    if (reply->element[REDIS_PUB_SUB_CHANNEL_INDEX]->type != REDIS_REPLY_STRING ||
        reply->element[REDIS_PUB_SUB_PAYLOAD_INDEX]->type != REDIS_REPLY_STRING)
    {
        SWSS_LOG_ERROR("Channel name or payload in PUB/SUB reply is not a string. Ignoring message.");
        return;
    }

    std::string channel_name = std::string(reply->element[REDIS_PUB_SUB_CHANNEL_INDEX]->str);
    std::string msg_payload = std::string(reply->element[REDIS_PUB_SUB_PAYLOAD_INDEX]->str);

    SWSS_LOG_DEBUG("Got message from channel '%s': %s", channel_name.c_str(), msg_payload.c_str());

    m_queue.push({channel_name, msg_payload});
}

void MultiNotificationConsumer::pop(std::string &channel, std::string &op, std::string &data, std::vector<FieldValueTuple> &values)
{
    SWSS_LOG_ENTER();

    if (m_queue.empty())
    {
        SWSS_LOG_ERROR("Notification queue is empty, can't pop.");
        throw std::runtime_error("Notification queue is empty, can't pop");
    }

    std::pair<std::string, std::string> queue_front = m_queue.front();
    m_queue.pop();

    channel = queue_front.first;
    std::string msg_payload = queue_front.second;

    values.clear();
    JSon::readJson(msg_payload, values); // Assuming payload is JSON

    if (values.empty()) {
        SWSS_LOG_ERROR("JSON parsing resulted in empty values for message from channel %s: %s", channel.c_str(), msg_payload.c_str());
        // Set op and data to indicate error or empty state, or re-throw
        op = "error";
        data = "JSON parse error or empty data";
        return;
    }

    // Similar to NotificationConsumer, op is the field of the first Key-Value pair,
    // data is its value. The rest are "values".
    FieldValueTuple fvt = values.at(0);
    op = fvField(fvt);
    data = fvValue(fvt);

    values.erase(values.begin()); // Remove the first element as it's now op/data
}

void MultiNotificationConsumer::pops(std::deque<ChannelKeyOpFieldsValuesTuple> &vckco)
{
    SWSS_LOG_ENTER();

    vckco.clear();
    size_t count = 0;
    while (!m_queue.empty() && count < POP_BATCH_SIZE)
    {
        std::string channel;
        std::string op;
        std::string data;
        std::vector<FieldValueTuple> values;

        // Use the pop method to ensure consistent processing and error handling
        try {
            pop(channel, op, data, values); // Updated pop call
            if (op == "error") { // Check if pop indicated an error (e.g. JSON parsing)
                SWSS_LOG_WARN("Skipping message from channel %s due to pop error (op: %s, data: %s)", channel.c_str(), op.c_str(), data.c_str());
            } else {
                vckco.emplace_back(channel, data, op, values); // Construct the new tuple
            }
            count++; // Increment count even for errors to avoid infinite loop on bad persistent data
        } catch (const std::runtime_error& e) {
            SWSS_LOG_ERROR("Error during pop in pops: %s. Skipping this message.", e.what());
            // If pop throws (e.g. queue became empty unexpectedly),
            // we should stop or skip.
            count++; // Ensure progress
        }
    }

    // If we haven't filled the batch, try to read more data if available.
    // This is similar to NotificationConsumer's aggressive read.
    if (vckco.size() < POP_BATCH_SIZE)
    {
        if (m_subscribe && m_subscribe->getContext()) {
            // Peek for more data in redis socket
            int rc = swss::peekRedisContext(m_subscribe->getContext());
            if (rc > 0) {
                try {
                    readData(); // Feed into internal queue
                } catch (const std::runtime_error& e) {
                    SWSS_LOG_ERROR("Error during readData in pops: %s", e.what());
                    // Don't re-throw, allow already popped items to be returned.
                }
            }
        }

        // Try to fill more after readData
        while (!m_queue.empty() && count < POP_BATCH_SIZE)
        {
            std::string channel;
            std::string op;
            std::string data;
            std::vector<FieldValueTuple> values;
            try {
                pop(channel, op, data, values); // Updated pop call
                if (op == "error") {
                     SWSS_LOG_WARN("Skipping message from channel %s due to pop error (op: %s, data: %s) after readData", channel.c_str(), op.c_str(), data.c_str());
                } else {
                    vckco.emplace_back(channel, data, op, values); // Construct the new tuple
                }
                count++;
            } catch (const std::runtime_error& e) {
                SWSS_LOG_ERROR("Error during pop in pops (after readData): %s. Skipping.", e.what());
                count++;
            }
        }
    }
}

int MultiNotificationConsumer::peek()
{
    SWSS_LOG_ENTER();
    if (!m_subscribe || !m_subscribe->getContext())
    {
        SWSS_LOG_ERROR("Subscription DB connector or context is null in peek.");
        return -1; // Error
    }

    if (m_queue.empty())
    {
        // Peek for more data in redis socket
        int rc = swss::peekRedisContext(m_subscribe->getContext());
        if (rc < 0) { // Error
            SWSS_LOG_ERROR("Error peeking redis context: %d", rc);
            return -1;
        }
        if (rc == 0) { // No data on socket
            return 0;
        }

        // rc > 0 means data is available on the socket
        try {
            readData(); // Feed into internal queue
        } catch (const std::runtime_error& e) {
            SWSS_LOG_ERROR("Error during readData in peek: %s", e.what());
            return -1; // Error reading data
        }
    }
    return m_queue.empty() ? 0 : 1;
}

void MultiNotificationConsumer::unsubscribe(const std::vector<std::string>& channels_to_unsubscribe)
{
    SWSS_LOG_ENTER();

    if (channels_to_unsubscribe.empty())
    {
        SWSS_LOG_INFO("No channels provided to unsubscribe from.");
        return;
    }

    if (!m_subscribe || !m_subscribe->getContext())
    {
        SWSS_LOG_ERROR("Subscription DB connector is not initialized. Cannot unsubscribe.");
        // Depending on desired error handling, could throw an exception.
        // For now, logging and returning to match some existing patterns.
        return;
    }

    std::string cmd = "UNSUBSCRIBE";
    std::vector<std::string> actually_unsubscribing_from; // Channels that are in m_channels and in channels_to_unsubscribe

    for (const auto& channel : channels_to_unsubscribe)
    {
        // Only try to unsubscribe if we think we are subscribed
        if (std::find(m_channels.begin(), m_channels.end(), channel) != m_channels.end()) {
            cmd += " " + channel;
            actually_unsubscribing_from.push_back(channel);
        } else {
            SWSS_LOG_INFO("Not currently subscribed to channel '%s', skipping unsubscription for it.", channel.c_str());
        }
    }

    // If all channels_to_unsubscribe were not in m_channels
    if (cmd == "UNSUBSCRIBE") { // Or check actually_unsubscribing_from.empty()
        SWSS_LOG_INFO("None of the provided channels were in the current subscription list to send UNSUBSCRIBE command.");
        return;
    }

    try
    {
        RedisReply r(m_subscribe, cmd, REDIS_REPLY_ARRAY); // Expecting batched replies

        // Verify unsubscribe replies from Redis
        // Each successful unsubscription yields a 3-element array:
        // ["unsubscribe", channel_name, num_remaining_subscriptions]
        for (size_t i = 0; i < actually_unsubscribing_from.size(); ++i)
        {
            const std::string& channel_being_unsubscribed = actually_unsubscribing_from[i];
            redisReply *raw_reply = r.getReply(); // Get the current part of the batched reply

            if (!raw_reply || raw_reply->type != REDIS_REPLY_ARRAY || raw_reply->elements != 3)
            {
                SWSS_LOG_ERROR("Unexpected reply format during UNSUBSCRIBE for channel %s", channel_being_unsubscribed.c_str());
            }
            else if (std::string(raw_reply->element[0]->str) != "unsubscribe" ||
                     std::string(raw_reply->element[1]->str) != channel_being_unsubscribed)
            {
                SWSS_LOG_ERROR("Unexpected content in UNSUBSCRIBE reply for channel %s (got type '%s' for channel '%s')",
                               channel_being_unsubscribed.c_str(),
                               raw_reply->element[0]->str ? raw_reply->element[0]->str : "null",
                               raw_reply->element[1]->str ? raw_reply->element[1]->str : "null");
            }
            else
            {
                SWSS_LOG_INFO("Successfully received UNSUBSCRIBE confirmation from Redis for channel: %s. Remaining subscriptions reported by Redis: %lld",
                              channel_being_unsubscribed.c_str(), raw_reply->element[2]->integer);
            }

            if (i < actually_unsubscribing_from.size() - 1) { // If there are more replies expected for this command
                r.next(); // Advance to the next part of the batched reply.
            }
        }
        SWSS_LOG_INFO("Processed UNSUBSCRIBE command for %zu channels.", actually_unsubscribing_from.size());
    }
    catch (const std::exception &e)
    {
        SWSS_LOG_ERROR("Exception during UNSUBSCRIBE command: %s", e.what());
        // Even if command fails, proceed to update m_channels based on what we intended to unsubscribe.
    }
    catch (...)
    {
        SWSS_LOG_ERROR("Unknown exception during UNSUBSCRIBE command.");
        // Proceed to update m_channels
    }

    // Update internal list of subscribed channels using remove_if and find
    m_channels.erase(
        std::remove_if(m_channels.begin(), m_channels.end(),
                       [&channels_to_unsubscribe](const std::string& current_channel) {
                           return std::find(channels_to_unsubscribe.begin(),
                                            channels_to_unsubscribe.end(),
                                            current_channel) != channels_to_unsubscribe.end();
                       }),
        m_channels.end());

    SWSS_LOG_INFO("Number of active subscriptions in m_channels now: %zu", m_channels.size());
    if (m_channels.empty()) {
        SWSS_LOG_WARN("MultiNotificationConsumer is no longer subscribed to any channels internally.");
        // Note: The m_subscribe connection remains open but idle.
        // It will be closed when the MultiNotificationConsumer is destructed.
    }
}

} // namespace swss
