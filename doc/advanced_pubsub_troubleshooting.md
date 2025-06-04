# Advanced Pub/Sub Troubleshooting Guide

This guide provides tips for troubleshooting common issues with the `AdvancedProducerTable` and `AdvancedConsumerTable` system.

## General Checks & Tools

1.  **Redis Connectivity**:
    *   Use the `isRedisConnected()` method available on both `AdvancedProducerTable` and `AdvancedConsumerTable` instances.
    *   From the host or container, use `redis-cli PING`. It should return "PONG".
    *   Check network connectivity to the Redis instance if it's remote.
2.  **Logs**:
    *   The advanced Pub/Sub components use `SWSS_LOG_*` macros. Check the system logs (e.g., syslog, journalctl) for relevant messages.
    *   Log verbosity can often be controlled at the SWSS logger level for more detailed output if needed.
3.  **Configuration Verification**:
    *   Dump the `ADVANCED_PUBSUB_CONFIG` table from `CONFIG_DB` to verify settings.
        ```bash
        sonic-db-cli CONFIG_DB HGETALL "ADVANCED_PUBSUB_CONFIG|your_table_name"
        sonic-db-cli CONFIG_DB HGETALL "ADVANCED_PUBSUB_CONFIG|global"
        ```
    *   Ensure `loadPubSubConfig` is being called with the correct `ConfigDBConnector` and `tableName` if you expect specific configurations to load.
4.  **Redis Monitoring**:
    *   `redis-cli MONITOR`: Shows all commands processed by Redis. Use with caution in production as it impacts performance, but very useful for debugging.
    *   `redis-cli INFO`: Provides general Redis status and statistics.
    *   Check Redis memory usage (`INFO memory`) and persistence status.
5.  **Counters**:
    *   Inspect counters in `COUNTERS_DB` in the `PUBSUB_COUNTERS` hash.
        ```bash
        sonic-db-cli COUNTERS_DB HGETALL PUBSUB_COUNTERS
        ```
    *   Look for non-zero values in error-related counters (e.g., `producer_ack_timeout`, `consumer_nack_total`).
    *   Queue depth counters (`producer_queue_depth`, `consumer_outgoing_queue_depth`) can indicate bottlenecks.

## Producer Issues (`AdvancedProducerTable`)

*   **Messages Not Sent / Not Appearing for Consumer**:
    *   **Did you call `producer.flushPriorityQueue()`?** Messages are queued internally in `AdvancedProducerTable` until `flushPriorityQueue()` is called.
    *   Check producer application logs for any errors during `set()` or `flushPriorityQueue()`.
    *   **Redis Writable?**: Is Redis instance writable? Check `redis-cli INFO` for `read_only` mode or disk full issues. Check Redis logs.
    *   **Connectivity**: Use `producer.isRedisConnected()`.
    *   **Counters**: Check `producer_published_count` for your table and priority. Is it increasing when you `set()`?
*   **`waitForAck()` Times Out or Returns False Unexpectedly**:
    *   **Consumer Status**: Is the consumer application running, connected, and actively processing messages for the specific table?
    *   **Consumer ACKing**: Is the consumer correctly calling `consumer.ack(message_id)` for each message? Verify `message_id` matches.
    *   **Delivery Mode**: Was the message sent with `AT_LEAST_ONCE` or `EXACTLY_ONCE`? `waitForAck` is only meaningful for these modes. Check `producer.setDeliveryMode()` or the message's `delivery_mode`.
    *   **Correlation ID**: Does the `correlation_id` used in `waitForAck()` exactly match the one used in `producer.set()`?
    *   **ACK Timeout Config (`ack_timeout_ms`)**: Is the `ack_timeout_ms` (in `PubSubConfig`, checked by consumer's `processTimedOutMessages`) too short for the consumer's typical processing time?
    *   **Redis State Inspection**:
        *   `SMEMBERS CORR_IDS:<correlation_id>`: Does this set contain the expected message IDs?
        *   For each message ID from above: `HGETALL MSG_STATUS:<message_id>`:
            *   Is the status "PENDING"?
            *   Is the timestamp old (indicating it might have been processed by `processTimedOutMessages`)?
        *   `ZRANGE PENDING_MESSAGES_BY_TIME_KEY 0 -1 WITHSCORES`: Are the message IDs present here? Are their scores (timestamps) very old?
    *   **Consumer `processTimedOutMessages()`**: Is this method being called periodically by the consumer application? If not, timed-out messages won't be retried or moved to DLQ, potentially causing `waitForAck` to hang until its own timeout.
    *   **Counters**: Check `producer_ack_timeout` counter.
*   **High `producer_queue_depth` Counter**:
    *   Indicates messages are being enqueued in `AdvancedProducerTable` faster than `flushPriorityQueue()` can send them, or `flushPriorityQueue()` is not being called frequently enough.
    *   Could also indicate performance issues with Redis `XADD` (if underlying `ProducerTable` uses streams) or other Redis commands.

## Consumer Issues (`AdvancedConsumerTable`)

*   **Not Receiving Messages**:
    *   **Producer Sending?**: Verify the producer is sending messages to the correct table name and flushing its queue.
    *   **Connectivity**: Use `consumer.isRedisConnected()`.
    *   **Filters**: Are filters (key, content, custom) too restrictive or misconfigured? Temporarily disable filters to isolate.
    *   **`pops()` Call**: Is `consumer.pops()` being called?
    *   **Select Loop (if used)**: If using `swss::Select`, is the loop working correctly? Is `consumer.getFd()` added to `Select`? Is `consumer.readData()` (or `consumer.updateAfterRead()`) called when `select()` indicates activity?
    *   **Deserialization Errors**: Check consumer logs for "Failed to deserialize message" errors. Malformed messages are moved to DLQ. Inspect DLQ content.
    *   **Counters**:
        *   `consumer_outgoing_queue_depth`: If high, messages are fetched from Redis into the consumer's internal processed queue but not popped by the application.
        *   `consumer_filter_miss`: If high, filters are rejecting messages.
*   **Messages Incorrectly Filtered**:
    *   Double-check filter patterns and logic (`setKeyFilter`, `setContentFilter`, custom predicate).
    *   Add detailed logging within your custom filter predicate if used.
    *   Verify `consumer_filter_match` and `consumer_filter_miss` counters.
*   **Messages Going to DLQ Unexpectedly**:
    *   **Inspect DLQ**:
        *   The DLQ for table `MY_TABLE` is typically `MY_TABLE_DLQ`.
        *   Messages in DLQ are stored with their original message ID as the key (e.g., `MY_TABLE_DLQ:<original_message_id>`).
        *   The value is a HASH with one field: `advanced_message_payload`, which contains the JSON string of the full `common::Message` object (including original content and DLQ metadata).
        *   Fetch and parse this payload: `HGET MY_TABLE_DLQ:<original_message_id> advanced_message_payload`.
        *   Check `dlq_reason`, `last_nack_error_message`, `retry_count` fields in the deserialized message.
    *   **Common Reasons**:
        *   Deserialization failure of incoming message.
        *   Message NACKed by application and `requeue` was false or `max_retries` exceeded.
        *   Message timed out waiting for ACK and `max_retries` exceeded via `processTimedOutMessages()`.
    *   **Counters**: Check `consumer_dlq_sent`, `consumer_nack_to_dlq`, `consumer_timeout_to_dlq`.
*   **Manual ACKs/NACKs Not Behaving as Expected**:
    *   **`enableManualAck(true)`**: Was this called on the consumer?
    *   **Correct `message_id`**: Ensure `consumer.ack(msg.id)` or `consumer.nack(msg.id, ...)` is called with the `id` from the consumed `swss::Message` object.
    *   **Redis Status**: Check `MSG_STATUS:<message_id>` in `STATE_DB` (or relevant DB) to see if "ACKED" or "NACKED" (or "PENDING" for requeue) is being set.
    *   **`PENDING_MESSAGES_BY_TIME_KEY`**: Check if ACKed/NACKed messages are correctly removed/updated in this sorted set.
    *   **Counters**: Check `consumer_ack_success`, `consumer_nack_total`, `consumer_nack_requeued`, `consumer_nack_to_dlq`.
*   **`processTimedOutMessages()` Not Working**:
    *   Is it being called periodically by the application?
    *   Is `ack_timeout_ms` in `PubSubConfig` set appropriately?
    *   Are PENDING messages correctly added to `PENDING_MESSAGES_BY_TIME_KEY` by the producer?
    *   Check logs for errors during its execution.
*   **High `consumer_outgoing_queue_depth` Counter**:
    *   Application's `pops()` calls are not keeping up with message processing rate from Redis into the internal queue.
    *   Application might be too slow in its message handling loop.

## DLQ Management Issues

*   **DLQ Growing Uncontrollably**:
    *   This indicates persistent issues causing messages to fail. Address the root cause (see "Messages Going to DLQ Unexpectedly").
    *   **`trimDeadLetterQueue()`**: Is this method being called periodically if you expect old DLQ messages to be removed? Note that the current implementation is a placeholder and warns about inefficiency with the HASH-based DLQ. A more robust DLQ structure (e.g., Redis List or Sorted Set for the DLQ itself) would be needed for efficient, automated trimming.
    *   Check `dead_letter_retention_hours` in `PubSubConfig`.
*   **Message Replay (`popMessagesForReplay`) Not Working**:
    *   This method is currently a placeholder due to the DLQ structure. It warns about this and returns an empty vector.
    *   A proper implementation would require a mechanism to list message IDs from the DLQ in a defined order (e.g., oldest first), which is non-trivial with the current HASH-per-message DLQ storage.
    *   For manual replay, you would need to:
        1.  Identify message IDs in the DLQ (e.g., by `SCAN` if keys are `DLQ_TABLE_NAME:<msg_id>`).
        2.  For each ID, `HGET DLQ_TABLE_NAME:<msg_id> advanced_message_payload`.
        3.  Deserialize the JSON string to a `common::Message`.
        4.  Prepare it for replay (clear DLQ fields, reset retry count, potentially clear original ID if producer generates new ones).
        5.  Send it to the appropriate original table using an `AdvancedProducerTable` instance.
        6.  Manually delete the replayed message from the DLQ HASH.
```
