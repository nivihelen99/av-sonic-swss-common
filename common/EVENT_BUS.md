# Common In-Process EventBus (`common::EventBus`)

The `common::EventBus` provides a lightweight, in-process publish-subscribe mechanism for C++ applications within the SWSS common libraries. It allows different components within the same process to communicate asynchronously through events.

## Features

-   **Topic-based:** Events are published to named topics (strings).
-   **Typed Event Data:** Event data is carried by `std::any`, allowing any data type to be sent. Subscribers are responsible for knowing the expected data type.
-   **Callbacks:** Subscribers register callback functions (`std::function`) to be invoked when an event is received.
-   **Filtering:** Subscribers can optionally provide filter functions to process only relevant events on a topic.
-   **Subscription Management:** Subscription objects are returned, allowing for explicit unsubscription.
-   **Synchronous Dispatch:** Events are dispatched to subscribers on the same thread that calls `publish()`.

## API Overview

The main components are `common::EventBus`, `common::Event`, and `common::Subscription`.

### `common::Event`

Represents an event to be published or received.

```cpp
#include "common/eventbus.h"

// Event Structure
struct common::Event {
    std::string topic; // The topic this event pertains to.
    std::any data;     // The data payload of the event.

    Event(const std::string& t, std::any d);
};
```

### `common::EventBus`

The central class for managing topics, publishing, and subscribing.

```cpp
#include "common/eventbus.h"
#include <memory> // For std::shared_ptr

// Obtain an EventBus instance (typically as a shared_ptr)
std::shared_ptr<common::EventBus> bus = std::make_shared<common::EventBus>();
```

#### Publishing Events

```cpp
// Publish an event with a topic and data
std::string my_topic = "system.notifications";
std::string message = "System rebooting in 5 minutes.";
bus->publish(my_topic, message); // std::string will be wrapped in std::any

// Publish with a pre-constructed Event object
common::Event custom_event(my_topic, MyCustomDataType{123, "details"});
bus->publish(custom_event);
```

#### Subscribing to Topics

Subscribers provide a topic name and a callback function. An optional filter function can also be provided.

```cpp
// Callback function signature
using SubscriberCallback = std::function<void(const common::Event&)>;

// Filter function signature (optional)
// Returns true if the event should be delivered to this subscriber.
using EventFilter = std::function<bool(const common::Event&)>;

// Example 1: Simple subscription
auto subscription1 = bus->subscribe("system.notifications",
    [](const common::Event& event) {
        try {
            std::string msg = std::any_cast<std::string>(event.data);
            std::cout << "Received notification on topic '" << event.topic
                      << "': " << msg << std::endl;
        } catch (const std::bad_any_cast& e) {
            std::cerr << "Error casting event data: " << e.what() << std::endl;
        }
    }
);

// Example 2: Subscription with a filter
struct AlertData { int level; std::string message; };
auto subscription2 = bus->subscribe("system.alerts",
    [](const common::Event& event) {
        AlertData alert = std::any_cast<AlertData>(event.data);
        std::cout << "High priority alert: " << alert.message << std::endl;
    },
    [](const common::Event& event) { // Filter function
        try {
            const AlertData& alert = std::any_cast<const AlertData&>(event.data);
            return alert.level > 5; // Only process alerts with level > 5
        } catch (const std::bad_any_cast&) {
            return false;
        }
    }
);
```

#### Unsubscribing

The `subscribe` method returns a `std::shared_ptr<common::Subscription>`. This handle can be used to unsubscribe.

```cpp
if (subscription1) {
    bus->unsubscribe(subscription1);
    subscription1.reset(); // Good practice to reset the handle
}
```
If the `Subscription` object goes out of scope and its reference count drops to zero, it does *not* automatically unsubscribe. Explicit unsubscription via `EventBus::unsubscribe()` is required.

## Event Data Handling

Event data is stored in `std::any`. Subscribers must know the expected data type and use `std::any_cast` to retrieve it.

```cpp
void my_callback(const common::Event& event) {
    if (event.topic == "user.login") {
        std::string username = std::any_cast<std::string>(event.data);
        // process username
    } else if (event.topic == "sensor.reading") {
        SensorReading reading = std::any_cast<SensorReading>(event.data);
        // process reading
    }
}
```
It's crucial to handle potential `std::bad_any_cast` exceptions if data types might vary or are uncertain.

## Threading Model

The `common::EventBus` is synchronous. When `publish()` is called:
1. The `EventBus` identifies all subscribers for the given topic.
2. For each subscriber, if a filter exists, it's executed.
3. If the filter passes (or no filter exists), the subscriber's callback is executed.
All these actions happen on the thread that called `publish()`. The `publish()` call will only return after all relevant callbacks have completed.

This means:
- Callbacks should be relatively quick to avoid blocking the publisher.
- If a callback needs to perform long-running operations, it should delegate that work to another thread.
- The `EventBus` itself does not introduce any new threads.
- If multiple threads publish events concurrently, or if subscribe/unsubscribe operations occur concurrently with publishing, the user must ensure proper synchronization if shared data is accessed by callbacks or if subscriber lists are modified during iteration (though the current implementation uses `std::unordered_map` which has some internal thread-safety for read/write if iterators are not invalidated, direct concurrent modification of the same key or map structure is not safe). For complex multi-threaded scenarios, external locking around publish/subscribe calls might be necessary depending on the use case. (The current implementation of `publish` iterates over a copy of subscribers for a topic, which makes it safe against subscribe/unsubscribe during publish. Subscribe/unsubscribe operations involve map writes and should be synchronized if called from multiple threads).

## Example Usage

```cpp
#include "common/eventbus.h"
#include <iostream>
#include <string>
#include <memory>

struct CustomData {
    int id;
    std::string payload;
};

int main() {
    std::shared_ptr<common::EventBus> bus = std::make_shared<common::EventBus>();

    // Subscriber 1 (simple)
    auto sub1 = bus->subscribe("topicA", [](const common::Event& event) {
        std::cout << "Sub1 (TopicA): Received string: "
                  << std::any_cast<std::string>(event.data) << std::endl;
    });

    // Subscriber 2 (with custom data and filter)
    auto sub2 = bus->subscribe("topicB",
        [](const common::Event& event) { // Callback
            CustomData data = std::any_cast<CustomData>(event.data);
            std::cout << "Sub2 (TopicB): Received CustomData ID " << data.id
                      << ", Payload '" << data.payload << "'" << std::endl;
        },
        [](const common::Event& event) { // Filter
            try {
                return std::any_cast<const CustomData&>(event.data).id > 50;
            } catch (const std::bad_any_cast&) { return false; }
        }
    );

    // Publish events
    bus->publish("topicA", std::string("Hello from Topic A!"));
    bus->publish("topicB", CustomData{30, "Low ID"});    // Will be filtered out
    bus->publish("topicB", CustomData{70, "High ID"});   // Will be received by Sub2
    bus->publish("topicC", std::string("Nobody listening here.")); // No subscribers for topicC

    // Unsubscribe Sub1
    bus->unsubscribe(sub1);
    bus->publish("topicA", std::string("This won't be received by Sub1."));

    return 0;
}

```
This example demonstrates basic publishing, subscribing with and without filters, handling different data types, and unsubscribing.
```
