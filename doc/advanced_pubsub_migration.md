# Advanced Pub/Sub Migration Guide

This guide helps users of the existing `swss::ProducerTable` and `swss::ConsumerTable` (often interacting with Redis lists via `LPUSH`/`BRPOP` or streams via `XADD`/`XREADGROUP`) to migrate to or start using the new `swss::AdvancedProducerTable` and `swss::AdvancedConsumerTable` for enhanced features.

## Key Differences and New Features

*   **Message Object (`common::Message`)**: The advanced system uses a structured `common::Message` object that encapsulates data and metadata. This object is serialized and sent over Redis.
*   **Priority Queuing**: `AdvancedProducerTable` supports message priorities, sending higher priority messages first when `flushPriorityQueue()` is called.
*   **Reliable Delivery & ACKs**:
    *   Supports `AT_LEAST_ONCE` and `EXACTLY_ONCE` delivery modes.
    *   Consumers can (and usually should for reliable modes) manually `ack()` or `nack()` messages.
    *   Producers can use `waitForAck()` for synchronous confirmation if a `correlation_id` is used.
*   **Filtering**: `AdvancedConsumerTable` allows server-side filtering of messages based on:
    *   Key (glob patterns).
    *   Content (JSONPath expressions - currently a simplified key match).
    *   Custom application-defined predicates.
*   **Dead Letter Queues (DLQ)**: Failed messages (NACKed after retries, timed-out, or malformed) are automatically moved to a DLQ for later inspection or replay.
*   **Timeout Processing**: Consumers can periodically call `processTimedOutMessages()` to handle messages that were delivered but not ACKed/NACKed within a configured timeout.
*   **Configuration**: Granular control over features (delivery mode, timeouts, retries, etc.) via `CONFIG_DB` using global or per-table settings. See `advanced_pubsub_config.md`.
*   **Selectable Consumer**: `AdvancedConsumerTable` uses an `eventfd` for integration with `swss::Select`, allowing event-driven consumption from its processed message queue.
*   **Counters**: Both producer and consumer provide metrics written to `COUNTERS_DB`.

## Backward Compatibility

*   The existing `swss::ProducerTable` and `swss::ConsumerTable` APIs and behavior remain **unchanged**.
*   You can continue to use them as before for existing tables or new simple use cases.
*   The new advanced features are opt-in by choosing to use the new `AdvancedProducerTable` and `AdvancedConsumerTable` classes.
*   **Interoperability**:
    *   An `AdvancedProducerTable` sends messages in a specific format (a serialized `common::Message` within a Redis hash field). A standard `ConsumerTable` could read this raw data but would need custom deserialization to understand it.
    *   An `AdvancedConsumerTable` expects messages in this new format. It will likely fail to process (and move to DLQ) messages sent by a standard `ProducerTable` if they don't match the expected structure.
    *   **It is strongly recommended to migrate both producer and consumer for a given table/channel to the advanced versions if these features are desired.**

## Migrating to `AdvancedProducerTable`

1.  **Include Header**:
    ```cpp
    #include "common/advancedproducertable.h"
    #include "common/advanced_pubsub_types.h" // For common::DeliveryMode
    #include "common/configdb.h" // If passing ConfigDBConnector
    ```
2.  **Instantiation**:
    *   Old: `swss::ProducerTable producer(dbConnector, "MY_TABLE");`
    *   New: `swss::AdvancedProducerTable producer(dbConnector, "MY_TABLE", configDbConnector);`
        *   `configDbConnector` is an optional `swss::ConfigDBConnector*`. If `nullptr`, default `PubSubConfig` values are used.
3.  **Sending Messages**:
    *   The base `producer.set(key, fv);` and `producer.del(key);` from `ProducerTable` are still available via `using ProducerTable::set;`. These will bypass advanced features like priority, structured `common::Message` serialization, and ACK tracking. They interact with Redis as the base `ProducerTable` does.
    *   To use advanced features, use the new `set` overload:
        ```cpp
        std::vector<swss::FieldValueTuple> fv;
        fv.emplace_back("field1", "value1");
        // ...
        // Get default priority from loaded config (or 5 if no config)
        int priority = producer.m_config.default_priority;
        std::string correlation_id = ""; // Optional

        producer.set("mykey1", fv, priority, correlation_id);
        ```
    *   **Important**: Messages are queued internally. Call `producer.flushPriorityQueue();` to actually send them to Redis.
4.  **Delivery Mode**:
    *   The default delivery mode is taken from `PubSubConfig` (loaded from `CONFIG_DB` or struct defaults, typically `AT_LEAST_ONCE`).
    *   You can override it per producer instance:
        ```cpp
        producer.setDeliveryMode(common::DeliveryMode::AT_MOST_ONCE);
        ```
    *   This instance-default applies to messages sent via the advanced `set()` overload.
5.  **Acknowledgments**:
    *   If using `AT_LEAST_ONCE` or `EXACTLY_ONCE`, and you provided a `correlation_id` when sending, you can wait for ACKs:
        ```cpp
        if (producer.waitForAck(correlation_id, 5000 /* timeout ms */)) {
            // Batch acknowledged
        } else {
            // Batch timed out or one/more messages NACKed
        }
        ```

## Migrating to `AdvancedConsumerTable`

1.  **Include Header**:
    ```cpp
    #include "common/advancedconsumertable.h"
    #include "common/advanced_pubsub_types.h" // For swss::Message
    #include "common/configdb.h" // If passing ConfigDBConnector
    ```
2.  **Instantiation**:
    *   Old: `swss::ConsumerTable consumer(dbConnector, "MY_TABLE");`
    *   New: `swss::AdvancedConsumerTable consumer(dbConnector, "MY_TABLE", popBatchSize, configDbConnector);`
        *   `popBatchSize` is optional (defaults to `DEFAULT_ADVANCED_POP_BATCH_SIZE`).
        *   `configDbConnector` is optional.
3.  **Receiving Messages**:
    *   The old `consumer.pop(kco);` or `consumer.pops(vkco);` (where `vkco` is `std::deque<KeyOpFieldsValuesTuple>`) are still available (overridden). If used, they will return messages by converting the internal `common::Message` back to `KeyOpFieldsValuesTuple`. The `kco.third` (values) will contain the original `FieldValueTuple` vector that was part of the `common::Message::content`.
    *   The new recommended method for `AdvancedConsumerTable` is:
        ```cpp
        std::deque<swss::Message> messages;
        consumer.pops(messages); // Gets fully deserialized swss::Message objects
        // or consumer.pops(messages, desired_max_count);
        ```
        Each `swss::Message` object in the deque contains all metadata (priority, ID, original key, original table, etc.) and the content (as a JSON string of the original `FieldValueTuple` vector).
4.  **Filtering**:
    *   Apply filters after instantiation if needed:
        ```cpp
        consumer.setKeyFilter("prefix_*");
        // consumer.setContentFilter("path.to.field", "expected_value"); // Simplified JSONPath for now
        // consumer.setCustomFilter([](const common::Message& msg) { return msg.priority > 5; });
        ```
5.  **Acknowledgments (Manual)**:
    *   If the messages are produced with `AT_LEAST_ONCE` or `EXACTLY_ONCE`, manual acknowledgment is highly recommended.
    ```cpp
    consumer.enableManualAck(true);
    // ... in message processing loop ...
    for (const auto& msg : messages) {
        // ... process msg ...
        if (processing_ok) {
            consumer.ack(msg.id);
        } else {
            consumer.nack(msg.id, true /* requeue? */, "Error details");
        }
    }
    ```
6.  **Selectable Interface**:
    *   `AdvancedConsumerTable` is a `swss::Selectable`. Use `consumer.getFd()` and integrate into your `swss::Select` loop for event-driven consumption. Call `consumer.readData()` or `consumer.updateAfterRead()` when `Select` indicates activity on this consumer's FD.
7.  **Timeout Processing**:
    *   Periodically call `consumer.processTimedOutMessages();` in your application's main loop or a dedicated thread to handle messages that were delivered but not ACKed/NACKed within the configured `ack_timeout_ms`.

## Configuration
Review `advanced_pubsub_config.md` for details on configuring features like default delivery mode, ACK timeouts, max retries, and DLQ retention via `CONFIG_DB`. If no configuration is provided, the system uses sensible defaults defined in `swss::PubSubConfig`.
