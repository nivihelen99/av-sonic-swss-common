# Topic-Based Event and Notification Filtering

## 1. Overview of Topic-Based Filtering

When working with event-driven architectures, subscribers often need to process only a subset of the messages published on a channel or event stream. Traditionally, this might require publishers to create many specific channels or for subscribers to receive all messages and discard irrelevant ones, leading to inefficiency.

Topic-based filtering provides a client-side solution to this problem. Publishers tag messages with a specific "entity ID" or "topic." Subscribers can then specify which topics they are interested in. The new subscriber wrapper classes, `TopicEventSubscriber` (for ZMQ-based events) and `FilteredNotificationConsumer` (for Redis Pub/Sub notifications), will then filter messages based on these subscriptions, delivering only relevant messages to the application.

This approach allows for more flexible and efficient message consumption, as filtering happens before the message is fully processed by the application logic.

## 2. For Publishers

To enable topic-based filtering, publishers need to include a special field in their messages that identifies the "topic" or "entity" the message pertains to.

### Using the `events.h` System (ZMQ-based events)

When publishing events using the `event_publish` function from `events.h`, the topic information should be added to the `event_params_t` map.

*   **Guidance:** Use the constant `EVENT_PARAM_ENTITY_ID` (defined in `common/events.h` as `"entity_id"`) as the key in your `event_params_t` map. The value associated with this key will be the topic string.

*   **C++ Code Example:**
    ```cpp
    #include "common/events.h"
    #include <string>
    #include <map>

    void publish_interface_event(event_handle_t publisher_handle, const std::string& interface_name, const std::string& status) {
        event_params_t params;
        params[EVENT_PARAM_ENTITY_ID] = interface_name; // e.g., "Ethernet0"
        params["status"] = status;                     // e.g., "up"
        // Add other relevant parameters
        // params["admin_status"] = "up";

        int ret = event_publish(publisher_handle, "interface_status_update", &params);
        if (ret != 0) {
            // Handle error
            SWSS_LOG_ERROR("Failed to publish interface event for %s", interface_name.c_str());
        }
    }
    ```

### Using `NotificationProducer` (Redis Pub/Sub)

When publishing notifications using `NotificationProducer`, the topic information should be added as one of the `FieldValueTuple` entries in the vector of fields.

*   **Guidance:** Use the constant `JSON_FIELD_ENTITY_ID` (defined in `common/filterednotificationconsumer.h` as `"_entity_id"`) as the field name in a `FieldValueTuple`. The value of this tuple will be the topic string. The underscore prefix for `_entity_id` helps avoid potential conflicts with schema-defined field names in Redis tables.

*   **C++ Code Example:**
    ```cpp
    #include "common/notificationproducer.h"
    #include "common/dbconnector.h"
    #include "common/filterednotificationconsumer.h" // For JSON_FIELD_ENTITY_ID

    void publish_vlan_notification(swss::NotificationProducer& producer, const std::string& vlan_name, const std::string& state) {
        std::vector<swss::FieldValueTuple> values;
        values.emplace_back(swss::JSON_FIELD_ENTITY_ID, vlan_name); // e.g., "Vlan100"
        values.emplace_back("state", state);                        // e.g., "active"
        // Add other relevant fields
        // values.emplace_back("tagging_mode", "tagged");

        producer.send("VLAN_CHANGE", "CONFIG_VLAN_" + vlan_name, values);
    }
    ```

## 3. For Subscribers

Subscribers use the new wrapper classes to manage topic subscriptions and receive filtered messages.

### Using `TopicEventSubscriber` (for `events.h` system)

*   **Purpose:** `TopicEventSubscriber` wraps the low-level `events.h` C-API for subscribing to ZMQ-based events. It manages topic subscriptions and filters incoming events based on the `EVENT_PARAM_ENTITY_ID` field.

*   **API Summary:**
    *   `TopicEventSubscriber(const std::string& event_source, bool use_cache = false, int recv_timeout = -1, const event_subscribe_sources_t *sources = nullptr)`: Constructor. `event_source` is a hint if `sources` is `nullptr`.
    *   `~TopicEventSubscriber()`: Destructor.
    *   `void subscribeTopic(const std::string& entity_topic)`: Subscribes to messages tagged with `entity_topic`.
    *   `void unsubscribeTopic(const std::string& entity_topic)`: Unsubscribes from `entity_topic`.
    *   `bool isTopicSubscribed(const std::string& entity_topic) const`: Checks if subscribed to a topic.
    *   `int receive(event_receive_op_t& evt, int timeout_ms = -1)`: Receives a filtered event. Populates `evt`. `timeout_ms` can override the constructor's default. Returns 0 on success, >0 on timeout, <0 on error.

*   **Short C++ Example:**
    ```cpp
    #include "common/topiceventsubscriber.h"
    #include "common/logger.h" // For SWSS_LOG_INFO

    void process_interface_events() {
        swss::TopicEventSubscriber subscriber("sonic-events-port", false, 1000); // Source hint, 1s timeout

        subscriber.subscribeTopic("Ethernet0");
        subscriber.subscribeTopic("Ethernet4");

        swss::event_receive_op_t event_data;
        int result = subscriber.receive(event_data);

        if (result == 0) {
            SWSS_LOG_INFO("Received event for key: %s, entity_id: %s",
                          event_data.key.c_str(),
                          event_data.params[EVENT_PARAM_ENTITY_ID].c_str());
            // Process event_data.params
        } else if (result > 0) {
            SWSS_LOG_INFO("Timeout waiting for interface event.");
        } else {
            SWSS_LOG_ERROR("Error receiving interface event: %d", result);
        }
    }
    ```

*   **Short Python Example:**
    ```python
    from swsscommon import swsscommon

    def handle_port_events_py():
        # Parameters: event_source (conceptual if sources list provided), use_cache, recv_timeout, sources_list
        subscriber = swsscommon.TopicEventSubscriber("sonic-events-port", False, 1000, ["sonic-events-port"])

        subscriber.subscribeTopic("Ethernet0")
        subscriber.subscribeTopic("Ethernet8")
        print(f"Is Ethernet0 subscribed? {subscriber.isTopicSubscribed('Ethernet0')}")

        evt_op = swsscommon.event_receive_op_t()

        # The receive call takes the event_receive_op_t instance and an optional timeout override
        # Default timeout is from constructor (1000ms here)
        # Override timeout to 500ms for this specific call:
        return_code = subscriber.receive(evt_op, 500)

        if return_code == 0:
            print(f"Received key: {evt_op.key}")
            print(f"Params: {dict(evt_op.params)}")
            entity_id = evt_op.params.get(swsscommon.EVENT_PARAM_ENTITY_ID, "N/A")
            print(f"Entity ID: {entity_id}")
        elif return_code > 0:
            print("Timeout receiving event.")
        else:
            print(f"Error receiving event: {return_code}")

    # handle_port_events_py()
    ```

### Using `FilteredNotificationConsumer` (for Redis Pub/Sub)

*   **Purpose:** `FilteredNotificationConsumer` extends `Selectable` and wraps `NotificationConsumer` to provide topic-based filtering for Redis Pub/Sub messages. It filters messages based on the `JSON_FIELD_ENTITY_ID` field.

*   **API Summary:**
    *   `FilteredNotificationConsumer(swss::DBConnector *db, const std::string &channel, int pri = 0, size_t popBatchSize = NotificationConsumer::DEFAULT_POP_BATCH_SIZE)`: Constructor.
    *   `~FilteredNotificationConsumer()`: Destructor.
    *   `void subscribeTopic(const std::string& entity_topic)`: Subscribes to messages tagged with `entity_topic`.
    *   `void unsubscribeTopic(const std::string& entity_topic)`: Unsubscribes from `entity_topic`.
    *   `bool isTopicSubscribed(const std::string& entity_topic) const`: Checks if subscribed to a topic.
    *   `void pop(std::string &op, std::string &data, std::vector<FieldValueTuple> &values)`: Pops a single filtered message. Throws `std::runtime_error` if empty.
    *   `void pops(std::deque<KeyOpFieldsValuesTuple> &vkco)`: Pops all currently available filtered messages.
    *   Inherits `Selectable` methods: `getFd()`, `readData()`, `hasData()`, `hasCachedData()`, `peek()`.

*   **Short C++ Example:**
    ```cpp
    #include "common/filterednotificationconsumer.h"
    #include "common/dbconnector.h"
    #include "common/select.h"
    #include "common/logger.h" // For SWSS_LOG_INFO

    void process_vlan_notifications() {
        swss::DBConnector db("APPL_DB", 0);
        swss::FilteredNotificationConsumer vlan_updates(&db, "VLAN_NOTIFICATIONS");

        vlan_updates.subscribeTopic("Vlan100");
        vlan_updates.subscribeTopic("Vlan200");

        swss::Select s;
        s.addSelectable(&vlan_updates);

        swss::Selectable *sel = nullptr;
        int result = s.select(&sel, 1000); // 1 second timeout

        if (result == swss::Select::OBJECT && sel == &vlan_updates) {
            std::string op, data;
            std::vector<swss::FieldValueTuple> values;
            vlan_updates.pop(op, data, values);

            std::string entity_id_val = "N/A";
            for(const auto& fv : values) {
                if (fvField(fv) == swss::JSON_FIELD_ENTITY_ID) {
                    entity_id_val = fvValue(fv);
                    break;
                }
            }
            SWSS_LOG_INFO("VLAN op: %s, key: %s, entity: %s", op.c_str(), data.c_str(), entity_id_val.c_str());
            // Process 'values'
        } else if (result == swss::Select::TIMEOUT) {
            SWSS_LOG_INFO("Timeout waiting for VLAN notification.");
        } else {
            SWSS_LOG_ERROR("Error in select: %d", result);
        }
    }
    ```

*   **Short Python Example:**
    ```python
    from swsscommon import swsscommon
    import time

    def handle_vlan_updates_py():
        db = swsscommon.DBConnector("APPL_DB", 0, True) # ASIC_DB, timeout_ms, use_unix_socket
        consumer = swsscommon.FilteredNotificationConsumer(db, "VLAN_CHANNEL_PY")

        consumer.subscribeTopic("VLAN_PY_10")
        consumer.subscribeTopic("VLAN_PY_20")
        print(f"Is VLAN_PY_10 subscribed? {consumer.isTopicSubscribed('VLAN_PY_10')}")

        # Example: Producer (in another process or test setup)
        # producer = swsscommon.NotificationProducer(db, "VLAN_CHANNEL_PY")
        # fv = swsscommon.FieldValuePairs([(swsscommon.JSON_FIELD_ENTITY_ID, "VLAN_PY_10"), ("status", "active")])
        # producer.send("CONFIG_UPDATE", "VLAN_TABLE:VLAN_PY_10", fv)
        # time.sleep(0.01) # Give time for Redis pub/sub

        sel = swsscommon.Select()
        sel.addSelectable(consumer)

        # Timeout in milliseconds for select call
        state, s = sel.select(1000)

        if state == swsscommon.Select.OBJECT:
            op, data, values = consumer.pop() # Default pop timeout is 0 (non-blocking for pop itself after select)
                                            # The C++ pop() takes timeout as arg, Python binding might differ slightly or use default.
                                            # For FilteredNotificationConsumer, pop itself doesn't have timeout, it relies on select.

            entity_id = "N/A"
            # values is a list of tuples
            for field, value in values:
                if field == swsscommon.JSON_FIELD_ENTITY_ID:
                    entity_id = value
                    break
            print(f"Op: {op}, Data: {data}, Entity: {entity_id}")
            # print(f"All values: {list(values)}")
        elif state == swsscommon.Select.TIMEOUT:
            print("Python: Timeout waiting for VLAN notification.")
        else:
            print(f"Python: Error in select: {state}")

    # handle_vlan_updates_py()
    ```

This documentation provides a comprehensive guide for both publishers and subscribers on how to use the new topic-based filtering feature.
