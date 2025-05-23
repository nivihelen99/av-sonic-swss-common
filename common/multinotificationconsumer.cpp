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

    if (reply == nullptr) {
        SWSS_LOG_WARN("processReply called with null reply.");
        return;
    }

    if (reply->type != REDIS_REPLY_ARRAY) {
        SWSS_LOG_DEBUG("Reply is not ARRAY, type: %d. Content: %s. Ignoring.", 
                       reply->type, (reply->str ? reply->str : "N/A"));
        return;
    }

    if (reply->elements == 0) {
        SWSS_LOG_DEBUG("Reply is an empty array. Ignoring.");
        return;
    }
    
    if (reply->element[0]->type != REDIS_REPLY_STRING) {
        SWSS_LOG_DEBUG("First element of reply array is not a string (type: %d). Ignoring.", reply->element[0]->type);
        return;
    }
    std::string type_str = reply->element[0]->str;

    // Handle control message confirmations
    if (type_str == "subscribe" || type_str == "unsubscribe" || type_str == "psubscribe" || type_str == "punsubscribe")
    {
        std::string name_field = "N/A"; 
        if (reply->elements > 1 && reply->element[1]->type == REDIS_REPLY_STRING) {
            name_field = reply->element[1]->str;
        }
        long long current_subs_count = -1;
        if (reply->elements > 2 && reply->element[2]->type == REDIS_REPLY_INTEGER) {
            current_subs_count = reply->element[2]->integer;
        }
        SWSS_LOG_INFO("Received '%s' confirmation. Name: %s. Count: %lld. Ignoring this as data.", 
                      type_str.c_str(), name_field.c_str(), current_subs_count); // Changed to SWSS_LOG_INFO for visibility
        return; 
    }

    // Handle actual data messages: "message" or "pmessage"
    if (type_str == "message") 
    {
        if (reply->elements != REDIS_PUB_SUB_MESSAGE_ELEMENTS) 
        {
            SWSS_LOG_ERROR("Expected %d elements for 'message' type, got: %zu. Ignoring.",
                           REDIS_PUB_SUB_MESSAGE_ELEMENTS, reply->elements);
            return;
        }
        if (reply->element[REDIS_PUB_SUB_CHANNEL_INDEX]->type != REDIS_REPLY_STRING ||
            reply->element[REDIS_PUB_SUB_PAYLOAD_INDEX]->type != REDIS_REPLY_STRING)
        {
            SWSS_LOG_ERROR("'message' channel name or payload is not a string. Ignoring.");
            return;
        }
        std::string channel_name = std::string(reply->element[REDIS_PUB_SUB_CHANNEL_INDEX]->str);
        std::string msg_payload = std::string(reply->element[REDIS_PUB_SUB_PAYLOAD_INDEX]->str);
        SWSS_LOG_DEBUG("Got 'message' from channel '%s': %s", channel_name.c_str(), msg_payload.c_str());
        m_queue.push({channel_name, msg_payload});
    }
    else if (type_str == "pmessage") 
    {
        const int PMESSAGE_PATTERN_INDEX = 1;    
        const int PMESSAGE_CHANNEL_INDEX = 2;    
        const int PMESSAGE_PAYLOAD_INDEX = 3;    
        const size_t PMESSAGE_ELEMENTS = 4; 

        if (reply->elements != PMESSAGE_ELEMENTS)
        {
            SWSS_LOG_ERROR("Expected %zu elements for 'pmessage' type, got: %zu. Ignoring.",
                           PMESSAGE_ELEMENTS, reply->elements);
            return;
        }
        if (reply->element[PMESSAGE_PATTERN_INDEX]->type != REDIS_REPLY_STRING ||
            reply->element[PMESSAGE_CHANNEL_INDEX]->type != REDIS_REPLY_STRING ||
            reply->element[PMESSAGE_PAYLOAD_INDEX]->type != REDIS_REPLY_STRING)
        {
            SWSS_LOG_ERROR("'pmessage' pattern, actual channel name, or payload is not a string. Ignoring.");
            return;
        }
        std::string pattern_matched = std::string(reply->element[PMESSAGE_PATTERN_INDEX]->str);
        std::string actual_channel_name = std::string(reply->element[PMESSAGE_CHANNEL_INDEX]->str);
        std::string msg_payload = std::string(reply->element[PMESSAGE_PAYLOAD_INDEX]->str);
        SWSS_LOG_DEBUG("Got 'pmessage' via pattern '%s' from actual channel '%s': %s",
                       pattern_matched.c_str(), actual_channel_name.c_str(), msg_payload.c_str());
        
        m_queue.push({actual_channel_name, msg_payload});
    }
    else
    {
        SWSS_LOG_WARN("Received unhandled message type '%s' in PUB/SUB reply array. Elements: %zu. Content[1]: %s. Ignoring.",
                       type_str.c_str(), reply->elements, (reply->elements > 1 && reply->element[1]->str) ? reply->element[1]->str : "N/A");
    }
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

// Backward-compatible pop overload
void MultiNotificationConsumer::pop(std::string &op, std::string &data, std::vector<FieldValueTuple> &values)
{
    SWSS_LOG_ENTER();
    std::string dummy_channel; // To absorb the channel name
    // Call the primary pop method that includes channel output
    pop(dummy_channel, op, data, values);
    // The channel name in dummy_channel is ignored.
}

// Backward-compatible pops overload
void MultiNotificationConsumer::pops(std::deque<KeyOpFieldsValuesTuple> &vkco)
{
    SWSS_LOG_ENTER();
    vkco.clear();

    std::deque<ChannelKeyOpFieldsValuesTuple> internal_deque;
    // Call the primary pops method that takes ChannelKeyOpFieldsValuesTuple
    pops(internal_deque); 

    // Convert and transfer elements
    for (const auto& channel_tuple : internal_deque)
    {
        // ChannelKeyOpFieldsValuesTuple is std::tuple<std::string(channel), std::string(key), std::string(op), std::vector<FieldValueTuple>(values)>
        // KeyOpFieldsValuesTuple is std::tuple<std::string(key), std::string(op), std::vector<FieldValueTuple>(values)>
        vkco.emplace_back(std::get<1>(channel_tuple),  // key
                          std::get<2>(channel_tuple),  // op
                          std::get<3>(channel_tuple)); // values
    }
}

// Pops raw message payload string from a channel.
// Throws std::runtime_error if queue is empty.
std::string MultiNotificationConsumer::popPayload()
{
    SWSS_LOG_ENTER();
    if (m_queue.empty())
    {
        SWSS_LOG_ERROR("Notification queue is empty, can't pop payload.");
        throw std::runtime_error("Notification queue is empty, can't pop payload");
    }
    std::pair<std::string, std::string> entry = m_queue.front();
    m_queue.pop();
    return entry.second; // Return only the payload
}

// Pops raw (channel_name, message_payload) pair.
// Throws std::runtime_error if queue is empty.
std::pair<std::string, std::string> MultiNotificationConsumer::popWithChannel()
{
    SWSS_LOG_ENTER();
    if (m_queue.empty())
    {
        SWSS_LOG_ERROR("Notification queue is empty, can't pop with channel.");
        throw std::runtime_error("Notification queue is empty, can't pop with channel");
    }
    std::pair<std::string, std::string> entry = m_queue.front();
    m_queue.pop();
    return entry; // Return the pair (channel_name, payload)
}

void MultiNotificationConsumer::psubscribe(const std::vector<std::string>& patterns)
{
    SWSS_LOG_ENTER();
    if (patterns.empty())
    {
        SWSS_LOG_INFO("No patterns provided to psubscribe.");
        return;
    }

    if (!m_subscribe || !m_subscribe->getContext())
    {
        SWSS_LOG_ERROR("Subscription DB connector is not initialized. Cannot psubscribe.");
        throw std::runtime_error("Subscription DB connector not initialized in psubscribe");
    }

    std::string cmd = "PSUBSCRIBE";
    std::vector<std::string> added_patterns_this_call; // For potential rollback
    std::vector<std::string> patterns_in_command; // Patterns actually sent to Redis

    for (const auto& pattern : patterns)
    {
        // Only add if not already in m_patterns to avoid duplicate internal tracking
        // and redundant parts in the PSUBSCRIBE command if called with overlapping sets.
        if (std::find(m_patterns.begin(), m_patterns.end(), pattern) == m_patterns.end()) {
            cmd += " " + pattern;
            patterns_in_command.push_back(pattern);
            // Optimistically add to m_patterns, will be removed if subscription fails for this pattern
            // m_patterns.push_back(pattern); // This was the old optimistic add
            // added_patterns_this_call.push_back(pattern); // This was the old optimistic add
        } else {
            SWSS_LOG_INFO("Already psubscribed or request to psubscribe to pattern '%s' again, skipping in command.", pattern.c_str());
        }
    }
    
    if (patterns_in_command.empty()) {
        SWSS_LOG_INFO("No new patterns to psubscribe to (all were duplicates or initial list was effectively empty).");
        return;
    }

    try
    {
        RedisReply r(m_subscribe, cmd, REDIS_REPLY_ARRAY); 

        for (size_t i = 0; i < patterns_in_command.size(); ++i)
        {
            const std::string& current_pattern_in_cmd = patterns_in_command[i];
            redisReply *raw_reply = r.getReply();

            if (!raw_reply || raw_reply->type != REDIS_REPLY_ARRAY || raw_reply->elements != 3)
            {
                SWSS_LOG_ERROR("Unexpected reply format during PSUBSCRIBE for pattern '%s'", current_pattern_in_cmd.c_str());
                // No rollback from m_patterns here as it's not added yet for this pattern
                if (i < patterns_in_command.size() - 1) r.next();
                continue;
            }
            if (std::string(raw_reply->element[0]->str) != "psubscribe" ||
                std::string(raw_reply->element[1]->str) != current_pattern_in_cmd)
            {
                SWSS_LOG_ERROR("Unexpected content in PSUBSCRIBE reply for pattern '%s'. Got type '%s' for '%s'", 
                               current_pattern_in_cmd.c_str(), raw_reply->element[0]->str, raw_reply->element[1]->str);
                // No rollback from m_patterns here
                if (i < patterns_in_command.size() - 1) r.next();
                continue;
            }
            SWSS_LOG_INFO("Successfully psubscribed to pattern: %s", current_pattern_in_cmd.c_str());
            // Add to m_patterns only after successful Redis confirmation
            if (std::find(m_patterns.begin(), m_patterns.end(), current_pattern_in_cmd) == m_patterns.end()) {
                 m_patterns.push_back(current_pattern_in_cmd);
            }
            if (i < patterns_in_command.size() - 1) r.next();
        }
    }
    catch (const std::exception &e)
    {
        SWSS_LOG_ERROR("Exception during PSUBSCRIBE command: %s. m_patterns might be incomplete for this call.", e.what());
        // Rollback is implicitly handled as m_patterns is only updated on success per pattern.
        // No need for added_patterns_this_call anymore with per-pattern success model.
        throw; 
    }
}

void MultiNotificationConsumer::punsubscribe(const std::vector<std::string>& patterns_to_unsubscribe)
{
    SWSS_LOG_ENTER();
    if (patterns_to_unsubscribe.empty())
    {
        SWSS_LOG_INFO("No patterns provided to punsubscribe.");
        return;
    }

    if (!m_subscribe || !m_subscribe->getContext())
    {
        SWSS_LOG_ERROR("Subscription DB connector is not initialized. Cannot punsubscribe.");
        throw std::runtime_error("Subscription DB connector not initialized in punsubscribe");
    }

    std::string cmd = "PUNSUBSCRIBE";
    std::vector<std::string> patterns_in_cmd; 
    for (const auto& pattern : patterns_to_unsubscribe)
    {
        if (std::find(m_patterns.begin(), m_patterns.end(), pattern) != m_patterns.end()) {
            cmd += " " + pattern;
            patterns_in_cmd.push_back(pattern);
        } else {
            SWSS_LOG_INFO("Not currently psubscribed to pattern '%s', skipping punsubscription for it.", pattern.c_str());
        }
    }

    if (patterns_in_cmd.empty()) {
        SWSS_LOG_INFO("None of the provided patterns were in the current psubscription list to send PUNSUBSCRIBE command.");
        return;
    }

    try
    {
        RedisReply r(m_subscribe, cmd, REDIS_REPLY_ARRAY); 

        for (size_t i = 0; i < patterns_in_cmd.size(); ++i)
        {
            const std::string& current_pattern_in_cmd = patterns_in_cmd[i];
            redisReply* raw_reply = r.getReply();
            if (!raw_reply || raw_reply->type != REDIS_REPLY_ARRAY || raw_reply->elements != 3)
            {
                SWSS_LOG_ERROR("Unexpected reply format during PUNSUBSCRIBE for pattern %s", current_pattern_in_cmd.c_str());
                if (i < patterns_in_cmd.size() - 1) r.next();
                continue; 
            }
            if (std::string(raw_reply->element[0]->str) != "punsubscribe" ||
                std::string(raw_reply->element[1]->str) != current_pattern_in_cmd)
            {
                SWSS_LOG_ERROR("Unexpected content in PUNSUBSCRIBE reply for pattern '%s'. Got type '%s' for '%s'", 
                               current_pattern_in_cmd.c_str(), raw_reply->element[0]->str, raw_reply->element[1]->str);
                if (i < patterns_in_cmd.size() - 1) r.next();
                continue; 
            }
            SWSS_LOG_INFO("Successfully punsubscribed from pattern: %s (Redis confirmed)", current_pattern_in_cmd.c_str());
            m_patterns.erase(std::remove(m_patterns.begin(), m_patterns.end(), current_pattern_in_cmd), m_patterns.end());
            if (i < patterns_in_cmd.size() - 1) r.next();
        }
    }
    catch (const std::exception &e)
    {
        SWSS_LOG_ERROR("Exception during PUNSUBSCRIBE command: %s. m_patterns might be inconsistent.", e.what());
        throw;
    }
}

void MultiNotificationConsumer::unsubscribeAll()
{
    SWSS_LOG_ENTER();
    if (m_channels.empty()) {
        SWSS_LOG_INFO("Not subscribed to any channels, no need to unsubscribeAll.");
        return;
    }
    if (!m_subscribe || !m_subscribe->getContext()) {
        SWSS_LOG_ERROR("Subscription DB connector not initialized. Cannot unsubscribeAll.");
        throw std::runtime_error("Subscription DB connector not initialized in unsubscribeAll");
    }
    std::string cmd = "UNSUBSCRIBE"; 
    size_t initial_channel_count = m_channels.size(); 

    try {
        RedisReply r(m_subscribe, cmd, REDIS_REPLY_ARRAY); 
        for (size_t i = 0; i < initial_channel_count; ++i) {
            redisReply* raw_reply = r.getReply();
            if (!raw_reply || raw_reply->type != REDIS_REPLY_ARRAY || raw_reply->elements != 3 ||
                std::string(raw_reply->element[0]->str) != "unsubscribe") {
                SWSS_LOG_WARN("Unexpected or error reply during UNSUBSCRIBE ALL. Reply %zu of %zu. Chan: %s", 
                              i+1, initial_channel_count,
                              (raw_reply && raw_reply->elements > 1 && raw_reply->element[1]->type == REDIS_REPLY_STRING) ? raw_reply->element[1]->str : "unknown");
            } else {
                 SWSS_LOG_INFO("unsubscribeAll: Confirmation for channel %s.", raw_reply->element[1]->str);
            }
            if (i < initial_channel_count - 1) r.next();
        }
        m_channels.clear();
        SWSS_LOG_INFO("Unsubscribed from all channels (m_channels cleared).");
    } catch (const std::exception &e) {
        SWSS_LOG_ERROR("Exception during UNSUBSCRIBE ALL command: %s. m_channels might be inconsistent.", e.what());
        m_channels.clear(); // Clear internal list even if Redis interaction failed, to reflect intent
        throw;
    }
}

void MultiNotificationConsumer::punsubscribeAll()
{
    SWSS_LOG_ENTER();
    if (m_patterns.empty()) {
        SWSS_LOG_INFO("Not psubscribed to any patterns, no need to punsubscribeAll.");
        return;
    }
    if (!m_subscribe || !m_subscribe->getContext()) {
        SWSS_LOG_ERROR("Subscription DB connector not initialized. Cannot punsubscribeAll.");
        throw std::runtime_error("Subscription DB connector not initialized in punsubscribeAll");
    }
    std::string cmd = "PUNSUBSCRIBE"; 
    size_t initial_pattern_count = m_patterns.size(); 

    try {
        RedisReply r(m_subscribe, cmd, REDIS_REPLY_ARRAY); 
        for (size_t i = 0; i < initial_pattern_count; ++i) { 
            redisReply* raw_reply = r.getReply();
            if (!raw_reply || raw_reply->type != REDIS_REPLY_ARRAY || raw_reply->elements != 3 ||
                std::string(raw_reply->element[0]->str) != "punsubscribe") {
                 SWSS_LOG_WARN("Unexpected or error reply during PUNSUBSCRIBE ALL. Reply %zu of %zu. Pattern: %s",
                               i+1, initial_pattern_count,
                               (raw_reply && raw_reply->elements > 1 && raw_reply->element[1]->type == REDIS_REPLY_STRING) ? raw_reply->element[1]->str : "unknown");
            } else {
                SWSS_LOG_INFO("punsubscribeAll: Confirmation for pattern %s.", raw_reply->element[1]->str);
            }
            if (i < initial_pattern_count - 1) r.next();
        }
        m_patterns.clear();
        SWSS_LOG_INFO("Punsubscribed from all patterns (m_patterns cleared).");
    } catch (const std::exception &e) {
        SWSS_LOG_ERROR("Exception during PUNSUBSCRIBE ALL command: %s. m_patterns might be inconsistent.", e.what());
        m_patterns.clear(); // Clear internal list even if Redis interaction failed
        throw;
    }
}

} // namespace swss
