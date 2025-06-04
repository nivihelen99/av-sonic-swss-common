# Advanced Pub/Sub Configuration Guide

The Advanced Pub/Sub system can be configured via `CONFIG_DB`. Settings are applied per table (channel) or globally. Configuration entries are stored in a table named `ADVANCED_PUBSUB_CONFIG` (this is the value of `swss::CFG_ADVANCED_PUBSUB_TABLE_NAME`).

The key for a table-specific configuration within this table is the `<table_name>` itself (e.g., "MY_TABLE").
The key for global configuration is "global" (this is the value of `swss::CFG_ADVANCED_PUBSUB_GLOBAL_KEY`).

So, an entry in `CONFIG_DB` would look like:
`CONFIG_DB HMSET "ADVANCED_PUBSUB_CONFIG|MY_TABLE" default_priority 7 max_retries 5`
`CONFIG_DB HMSET "ADVANCED_PUBSUB_CONFIG|global" ack_timeout_ms 60000`

Table-specific settings override global settings if both are present for a particular parameter when `loadPubSubConfig` is called for that specific table. If no specific or global setting is found for a parameter for a table, the hardcoded default values within the `swss::PubSubConfig` struct are used.

## Configuration Parameters

The following parameters can be configured as fields in the hash entry for a given key:

*   **`default_priority`** (integer)
    *   Description: Default message priority if not specified in the producer's `set()` call. Used by `AdvancedProducerTable`.
    *   Range: 0 (lowest) to 9 (highest).
    *   Default: 5.
*   **`max_queue_depth`** (integer)
    *   Description: This setting is a placeholder from the original schema. For `AdvancedProducerTable`, messages are queued in an in-memory priority queue before being flushed. The underlying `ProducerTable` uses Redis Streams (`XADD`), and stream length can be managed with `MAXLEN`. This config field is not directly used by `AdvancedProducerTable`'s internal queue limiting yet but could be in the future, or for other producer types.
    *   Default: 10000.
*   **`ack_timeout_ms`** (integer)
    *   Description: Timeout in milliseconds for messages requiring acknowledgment (AT_LEAST_ONCE, EXACTLY_ONCE modes). If a consumer does not ACK/NACK a message within this period after its PENDING status is recorded/updated, `AdvancedConsumerTable::processTimedOutMessages()` will consider it timed out.
    *   Default: 30000 (30 seconds).
*   **`max_retries`** (integer)
    *   Description: Maximum number of retry attempts for a message that is NACKed with `requeue=true` or times out. After `max_retries`, the message will be moved to the Dead Letter Queue (DLQ) by the consumer.
    *   Default: 3.
*   **`delivery_mode_str`** (string)
    *   Description: Default delivery mode for messages published by an `AdvancedProducerTable` or expected by an `AdvancedConsumerTable` if not overridden programmatically. This string is parsed into the `delivery_mode_enum`.
    *   Values:
        *   `"at-most-once"`: Fire-and-forget. Messages might be lost. No ACKs.
        *   `"at-least-once"`: Guarantees delivery, duplicates are possible. Requires ACKs from consumer.
        *   `"exactly-once"`: Aims for single delivery. Requires ACKs. (Note: True system-wide exactly-once semantics are complex; this provides a strong at-least-once foundation with features to aid in duplicate handling).
    *   Default: `"at-least-once"`.
*   **`dead_letter_retention_hours`** (integer)
    *   Description: How long messages should be retained in the Dead Letter Queue (DLQ) for this table before being automatically trimmed by `AdvancedConsumerTable::trimDeadLetterQueue()`. A value of 0 or less disables automatic trimming. Note: The current `trimDeadLetterQueue` is a placeholder due to DLQ structure.
    *   Default: 168 (7 days).
*   **`enable_message_persistence`** (boolean: "true" or "false")
    *   Description: General flag indicating if persistence features (like ACK tracking in Redis) should be robustly used. The primary message queue (Redis Stream) is inherently persistent if Redis itself is configured for persistence (AOF/RDB). This flag primarily influences whether ACK/NACK metadata is written to Redis.
    *   Default: "true".
*   **`filter_cache_size`** (integer)
    *   Description: (Currently a placeholder) Intended for caching compiled filter expressions if complex filter types (e.g., advanced JSONPath or regex) were to be implemented that benefit from compilation. Not used by current filter implementations (glob, simple JSONPath key match).
    *   Default: 1000.

## Example `CONFIG_DB` entries:

To set a global acknowledgment timeout to 60 seconds and max retries to 5:
```
CONFIG_DB HMSET "ADVANCED_PUBSUB_CONFIG|global" ack_timeout_ms 60000 max_retries 5
```

To set specific settings for "MY_APP_TABLE", overriding global settings for this table:
```
CONFIG_DB HMSET "ADVANCED_PUBSUB_CONFIG|MY_APP_TABLE" default_priority 7 delivery_mode_str "exactly-once" max_retries 2
```
An `AdvancedProducerTable` or `AdvancedConsumerTable` instance for "MY_APP_TABLE" would use priority 7, exactly-once delivery, and 2 max retries. For other parameters like `ack_timeout_ms`, it would fall back to the global setting (60000ms) if a global config exists, or otherwise to the hardcoded default in `PubSubConfig`.
