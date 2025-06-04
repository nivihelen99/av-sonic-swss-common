# Advanced Pub/Sub Examples

This document provides examples for using `AdvancedProducerTable` and `AdvancedConsumerTable`.

## Basic Producer Example

```cpp
// Assuming dbConnector and configDbConnector are initialized
// #include "common/dbconnector.h"
// #include "common/configdb.h"
// #include "common/advancedproducertable.h"
// #include "common/advanced_pubsub_types.h" // For DeliveryMode

// swss::DBConnector* dbConnector = ...;
// swss::ConfigDBConnector* configDbConnector = ...; // Can be nullptr

swss::AdvancedProducerTable producer(dbConnector, "MY_TABLE", configDbConnector);

std::vector<swss::FieldValueTuple> fv;
fv.emplace_back("field1", "value1");
fv.emplace_back("field2", "value2");

// Publish with default priority (from config or internal default)
// and default delivery mode (from config or internal default AT_LEAST_ONCE)
producer.set("mykey1", fv, producer.m_config.default_priority); // Explicitly use configured default priority

// Publish with high priority
producer.set("mykey2", fv, 9);

// Publish with a correlation ID for ACK tracking
std::string corr_id = "my_batch_123";
producer.set("mykey3", fv, producer.m_config.default_priority, corr_id);
producer.set("mykey4", fv, producer.m_config.default_priority, corr_id);

producer.flushPriorityQueue(); // Send messages to Redis

// If delivery mode requires ACKs (e.g., AT_LEAST_ONCE, which is default)
if (producer.waitForAck(corr_id, 5000)) { // 5 second timeout
    // All messages in batch acknowledged
    // SWSS_LOG_INFO("Batch %s acknowledged", corr_id.c_str());
} else {
    // Batch timed out or was NACKed
    // SWSS_LOG_ERROR("Batch %s failed or timed out", corr_id.c_str());
}
```

## Basic Consumer Example (Polling)

```cpp
// Assuming dbConnector and configDbConnector are initialized
// #include "common/dbconnector.h"
// #include "common/configdb.h"
// #include "common/advancedconsumertable.h"
// #include "common/advanced_pubsub_types.h" // For swss::Message
// #include <iostream> // For std::cout
// #include <deque>    // For std::deque

// swss::DBConnector* dbConnector = ...;
// swss::ConfigDBConnector* configDbConnector = ...; // Can be nullptr

swss::AdvancedConsumerTable consumer(dbConnector, "MY_TABLE", 10, configDbConnector);

// Optional: Set filters
consumer.setKeyFilter("mykey*");
// consumer.setContentFilter("field1", "value1"); // Simplified; JSONPath would be more complex

// Enable manual acknowledgment if needed.
// For AT_LEAST_ONCE / EXACTLY_ONCE, manual ACK is often preferred.
// Default delivery mode from PubSubConfig is AT_LEAST_ONCE, so manual ACK is relevant.
consumer.enableManualAck(true);

std::deque<swss::Message> messages;
consumer.pops(messages); // Pops available messages based on priority and filtering

for (const auto& msg : messages) {
    // std::cout << "Received key: " << msg.original_key
    //           << " priority: " << msg.priority
    //           << " content: " << msg.content << std::endl;

    // Process message.content (which is a JSON string of FieldValueTuple vector)
    // std::vector<swss::FieldValueTuple> msg_content_fvs;
    // try {
    //    swss::Json content_json = swss::Json::parse(msg.content);
    //    swss::Json::deserialize(content_json, msg_content_fvs);
    //    for(const auto& fv : msg_content_fvs) {
    //        // std::cout << "  " << fvField(fv) << ": " << fvValue(fv) << std::endl;
    //    }
    // } catch (const std::exception& e) {
    //    // std::cerr << "Error deserializing message content: " << e.what() << std::endl;
    // }


    // If manual ACK is enabled:
    bool processed_successfully = true; // Determine this from your processing
    if (processed_successfully) {
        consumer.ack(msg.id);
    } else {
        consumer.nack(msg.id, true, "Processing failed, requeue."); // true to requeue (up to max_retries)
    }
}
```

## Consumer Example with Select (Event-Driven)
```cpp
// Assuming dbConnector, configDbConnector, and a swss::Select object are initialized
// swss::AdvancedConsumerTable consumer(dbConnector, "MY_TABLE", 10, configDbConnector);
// consumer.enableManualAck(true);
//
// swss::Select s;
// s.addSelectable(&consumer);
//
// while (true) {
//     swss::Selectable *sel;
//     int result = s.select(&sel, 1000); // 1 second timeout
//
//     if (result == swss::Select::TIMEOUT) {
//         // Handle timeout, maybe call consumer.processTimedOutMessages();
//         continue;
//     }
//     if (result == swss::Select::ERROR) {
//         // Handle error
//         break;
//     }
//
//     if (sel == &consumer) {
//         std::deque<swss::Message> messages;
//         consumer.pops(messages); // Consumer has data, pop it
//
//         for (const auto& msg : messages) {
//             // Process message as in polling example
//             // ...
//             bool processed_successfully = true;
//             if (processed_successfully) {
//                 consumer.ack(msg.id);
//             } else {
//                 consumer.nack(msg.id, true, "Processing failed via select, requeue.");
//             }
//         }
//     }
// }
```
(Note: The Select example is commented out as it requires a running loop context)
