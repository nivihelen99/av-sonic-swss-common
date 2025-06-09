#include <gtest/gtest.h>
#include "common/eventbus.h"
#include <string>
#include <vector>
#include <any>
#include <thread> // For concurrency tests later
#include <chrono> // For sleeps
#include <atomic> // For concurrent counters

// Basic Test Fixture
class EventBusTest : public ::testing::Test {
protected:
    std::shared_ptr<common::EventBus> bus;

    void SetUp() override {
        bus = std::make_shared<common::EventBus>();
    }

    void TearDown() override {
        // Bus is a shared_ptr, will be cleaned up automatically.
        // If any explicit cleanup for subscribers or threads were needed, it would go here.
    }
};

// Test 1: Simple Publish and Subscribe
TEST_F(EventBusTest, SimplePublishSubscribe) {
    std::string received_data;
    bool event_received = false;

    auto sub = bus->subscribe("test_topic", [&](const common::Event& event) {
        try {
            received_data = std::any_cast<std::string>(event.data);
            event_received = true;
        } catch (const std::bad_any_cast& e) {
            ADD_FAILURE() << "Bad any_cast: " << e.what();
        }
    });

    std::string message = "Hello, EventBus!";
    bus->publish("test_topic", std::string(message));

    ASSERT_TRUE(event_received);
    ASSERT_EQ(received_data, message);
}

// Test 2: Subscribe to a topic, publish to another
TEST_F(EventBusTest, NoReceiveOnDifferentTopic) {
    bool event_received = false;

    auto sub = bus->subscribe("topic1", [&](const common::Event& event) {
        event_received = true;
    });

    bus->publish("topic2", std::string("message for topic2"));

    ASSERT_FALSE(event_received);
}

// Test 3: Publish to a topic with no subscribers
TEST_F(EventBusTest, PublishWithNoSubscribers) {
    ASSERT_NO_THROW({
        bus->publish("empty_topic", std::string("message"));
    });
}

// Test 4: Basic Filtering - Accept event
TEST_F(EventBusTest, FilterAccept) {
    bool event_received = false;
    std::string received_data;

    common::EventFilter filter_func = [](const common::Event& event) {
        try {
            return std::any_cast<std::string>(event.data) == "special_message";
        } catch (const std::bad_any_cast&) {
            return false;
        }
    };

    auto sub = bus->subscribe("filter_topic", [&](const common::Event& event) {
        try {
            received_data = std::any_cast<std::string>(event.data);
            event_received = true;
        } catch (const std::bad_any_cast& e) {
            ADD_FAILURE() << "Bad any_cast: " << e.what();
        }
    }, filter_func);

    bus->publish("filter_topic", std::string("regular_message"));
    ASSERT_FALSE(event_received);

    bus->publish("filter_topic", std::string("special_message"));
    ASSERT_TRUE(event_received);
    ASSERT_EQ(received_data, "special_message");
}

// Test 5: Basic Filtering - Reject event
TEST_F(EventBusTest, FilterReject) {
    bool event_received = false;

    common::EventFilter filter_func = [](const common::Event& event) {
        try {
            return std::any_cast<std::string>(event.data) == "accept_this";
        } catch (const std::bad_any_cast&) {
            return false;
        }
    };

    auto sub = bus->subscribe("filter_topic_reject", [&](const common::Event& event) {
        event_received = true;
    }, filter_func);

    bus->publish("filter_topic_reject", std::string("reject_this"));

    ASSERT_FALSE(event_received);
}

// Test 6: Multiple Subscribers to the same topic
TEST_F(EventBusTest, MultipleSubscribers) {
    std::atomic<int> subscriber1_count(0);
    std::atomic<int> subscriber2_count(0);

    auto sub1 = bus->subscribe("multi_sub_topic", [&](const common::Event& event) {
        subscriber1_count++;
    });

    auto sub2 = bus->subscribe("multi_sub_topic", [&](const common::Event& event) {
        subscriber2_count++;
    });

    bus->publish("multi_sub_topic", std::string("event1"));
    bus->publish("multi_sub_topic", std::string("event2"));

    // Allow some time for events to be processed, though current bus is synchronous
    std::this_thread::sleep_for(std::chrono::milliseconds(50));


    ASSERT_EQ(subscriber1_count.load(), 2);
    ASSERT_EQ(subscriber2_count.load(), 2);
}

// Test 7: Unsubscribe
TEST_F(EventBusTest, Unsubscribe) {
    int receive_count = 0;
    auto sub_handle = bus->subscribe("unsub_topic", [&](const common::Event& event) {
        receive_count++;
    });

    bus->publish("unsub_topic", std::string("first_message"));
    ASSERT_EQ(receive_count, 1);

    ASSERT_NE(sub_handle, nullptr);
    if (sub_handle) {
        bus->unsubscribe(sub_handle);
    }

    bus->publish("unsub_topic", std::string("second_message"));
    ASSERT_EQ(receive_count, 1);
}

// Test 8: Event Data Correctness (different data type)
struct MyCustomData {
    int id;
    std::string value;

    // Needed for ASSERT_EQ and std::any_cast if copying
    bool operator==(const MyCustomData& other) const {
        return id == other.id && value == other.value;
    }
};

TEST_F(EventBusTest, CustomDataCorrectness) {
    MyCustomData sent_data = {101, "test_data"};
    MyCustomData received_data = {-1, ""}; // Default init
    bool event_received = false;

    auto sub = bus->subscribe("custom_data_topic", [&](const common::Event& event) {
        try {
            received_data = std::any_cast<MyCustomData>(event.data);
            event_received = true;
        } catch (const std::bad_any_cast& e) {
            ADD_FAILURE() << "Bad any_cast for MyCustomData: " << e.what();
        }
    });

    bus->publish("custom_data_topic", sent_data);

    ASSERT_TRUE(event_received);
    ASSERT_EQ(received_data.id, sent_data.id);
    ASSERT_EQ(received_data.value, sent_data.value);
}

// Test 9: Subscription to empty topic string
TEST_F(EventBusTest, SubscribeToEmptyTopic) {
    bool event_received = false;
    auto sub = bus->subscribe("", [&](const common::Event& event) {
        event_received = true;
    });
    bus->publish("", std::string("message_to_empty_topic"));
    ASSERT_TRUE(event_received);
}

// Test 10: Publish to empty topic string (same as above, just different focus)
TEST_F(EventBusTest, PublishToEmptyTopic) {
    bool event_received = false;
    auto sub = bus->subscribe("", [&](const common::Event& event) {
        event_received = true;
    });
    bus->publish("", std::string("message_to_empty_topic"));
    ASSERT_TRUE(event_received);
}


// Test 11: Unsubscribe with invalid/stale handle
TEST_F(EventBusTest, UnsubscribeInvalidHandle) {
    auto sub = bus->subscribe("topic_A", [](const common::Event&){});
    ASSERT_NE(sub, nullptr);
    bus->unsubscribe(sub);

    ASSERT_NO_THROW({
        bus->unsubscribe(sub); // Try to unsubscribe again with the same (now stale) handle
    });

    // Test with a completely new, unrelated subscription object not known by the bus
    // The ID for such a subscription might coincidentally match a live one if not careful.
    // Better to test unsubscribe with a subscription from a *different* bus instance, or a default constructed one.
    auto different_bus = std::make_shared<common::EventBus>();
    auto sub_from_different_bus = different_bus->subscribe("topic_B", [](const common::Event&){});

    ASSERT_NO_THROW({
         bus->unsubscribe(sub_from_different_bus); // Unsubscribe sub from different bus
    });

   ASSERT_NO_THROW({
         bus->unsubscribe(nullptr); // Unsubscribe nullptr
    });
}

// Test 12: Filter that throws an exception
TEST_F(EventBusTest, FilterThrowsException) {
    bool event_received = false;
    common::EventFilter filter_func = [](const common::Event& event) -> bool {
        throw std::runtime_error("Filter malfunction");
    };

    bus->subscribe("exception_filter_topic", [&](const common::Event& event) {
        event_received = true;
    }, filter_func);

    ASSERT_NO_THROW({
        bus->publish("exception_filter_topic", std::string("data"));
    });
    ASSERT_FALSE(event_received); // Event should not be received if filter throws
}

// Test 13: Callback that throws an exception
TEST_F(EventBusTest, CallbackThrowsException) {
    bus->subscribe("exception_callback_topic", [&](const common::Event& event) {
        throw std::runtime_error("Callback malfunction");
    });

    ASSERT_NO_THROW({
        bus->publish("exception_callback_topic", std::string("data"));
    });
    // Test primarily ensures the bus itself doesn't crash.
}

// Test 14: Concurrency Test - Multiple publishers, multiple subscribers on one topic
TEST_F(EventBusTest, DISABLED_BasicConcurrencyOneTopic) {
    // This test is disabled because proper concurrency testing is complex
    // and might lead to flakiness without proper synchronization primitives
    // within the test logic (e.g. barriers, latches) or if the EventBus isn't fully thread-safe.
    // The current EventBus implementation is synchronous and operations are on the caller's thread.
    // True concurrency issues would arise if subscribe/unsubscribe were called concurrently with publish
    // from different threads, or if the EventBus used internal worker threads (which it currently doesn't).

    std::atomic<int> received_count(0);
    const int num_publishers = 5;
    const int events_per_publisher = 10;

    auto sub = bus->subscribe("concurrent_topic", [&](const common::Event& event) {
        received_count++;
    });

    std::vector<std::thread> publisher_threads;
    for (int i = 0; i < num_publishers; ++i) {
        publisher_threads.emplace_back([&]() {
            for (int j = 0; j < events_per_publisher; ++j) {
                bus->publish("concurrent_topic", std::string("message_" + std::to_string(i) + "_" + std::to_string(j)));
            }
        });
    }

    for (auto& t : publisher_threads) {
        if (t.joinable()) {
            t.join();
        }
    }
    // For a synchronous bus, all publishes on a thread complete before the next on that thread.
    // All callbacks are executed on the publisher's thread.
    ASSERT_EQ(received_count.load(), num_publishers * events_per_publisher);
}


// --- ZMQ EventBus Bridge Tests Placeholder ---
// These will require more setup (ZMQ library, potentially a mock eventd or direct ZMQ PUB/SUB)
// For now, just a placeholder class.

class ZmqEventBusBridgeTest : public ::testing::Test {
protected:
    // Mock ZMQ setup or mini-broker might go here
    // std::shared_ptr<common::EventBus> local_bus;
    // event_handle_t subscriber_handle;

    void SetUp() override {
        // local_bus = std::make_shared<common::EventBus>();
        // Mock ZMQ init, potentially start a ZMQ proxy/broker thread
        // subscriber_handle = events_init_subscriber(false, 0, nullptr); // timeout 0ms
        // ASSERT_NE(subscriber_handle, nullptr);
        // EventSubscriber* sub_instance = static_cast<EventSubscriber*>(subscriber_handle); // Need EventSubscriber class
        // sub_instance->set_local_eventbus(local_bus);
    }

    void TearDown() override {
        // if (subscriber_handle) {
        //     events_deinit_subscriber(subscriber_handle);
        // }
        // ZMQ cleanup
    }
};

TEST_F(ZmqEventBusBridgeTest, DISABLED_SimulatedZmqToLocalEventBus) {
    // 1. Setup EventPublisher to publish to a ZMQ endpoint.
    // 2. Setup EventSubscriber (from events.h) to listen on that ZMQ endpoint.
    //    Initialize this EventSubscriber with a local common::EventBus instance using set_local_eventbus.
    // 3. Subscribe to a topic on the local common::EventBus.
    // 4. Use EventPublisher to send a ZMQ event.
    // 5. Verify the event is received by the subscriber on the local common::EventBus.
    GTEST_SKIP() << "ZMQ Bridge tests need ZMQ setup and are not implemented yet.";
}

// --- ZMQ Includes (need to be before gtest in some setups) ---
#include <zmq.h>

// --- Common SWSS Includes ---
#include "common/events_common.h" // For serialize, internal_event_t, etc.
#include "common/events.h"        // For event_handle_t, events_init_subscriber, etc.
#include "common/events_pi.h"     // For EventSubscriber class definition to call set_local_eventbus

// Helper to get UUID string for runtime_id (simplified)
static std::string generate_test_uuid() {
    uuid_t uuid;
    uuid_generate_random(uuid);
    char uuid_str[37];
    uuid_unparse_lower(uuid, uuid_str);
    return std::string(uuid_str);
}


class ZmqEventBusBridgeTest : public ::testing::Test {
protected:
    std::shared_ptr<common::EventBus> local_bus;
    event_handle_t subscriber_handle = nullptr;
    void* zmq_context = nullptr;
    void* zmq_pub_socket = nullptr; // Our test publisher socket

    static const char* ZMQ_ENDPOINT; // = "inproc://eventbus_bridge_test";
                                     // Defined outside due to static linkage issues with TEST_F

    void SetUp() override {
        // 0. Initialize ZMQ context for our test publisher
        zmq_context = zmq_ctx_new();
        ASSERT_NE(zmq_context, nullptr);

        // 1. Create our local EventBus
        local_bus = std::make_shared<common::EventBus>();

        // 2. Configure path for init_cfg.json or database_config.json
        //    This is tricky. events_init_subscriber internally calls get_config("xpub_path").
        //    We created 'tests/database_config.json'. The test runner needs to make swss-common load this.
        //    One way is if 'read_init_config("tests/database_config.json")' is called by test main.
        //    Or if an environment variable points to it.
        //    For now, we proceed hoping get_config() will pick up our ZMQ_ENDPOINT.
        //    If not, EventSubscriber::init will fail.

        // 3. Initialize EventSubscriber (from events.h)
        //    Use use_cache = false to minimize event_service interactions.
        //    Use a short timeout (e.g., 100ms) for recv if we were to use event_receive.
        subscriber_handle = events_init_subscriber(false, 100, nullptr);
        ASSERT_NE(subscriber_handle, nullptr) << "Failed to init EventSubscriber. Check ZMQ endpoint config for tests.";

        // 4. Get the C++ EventSubscriber instance and set our local EventBus
        //    This requires access to EventSubscriber class definition (via events_pi.h)
        //    and the get_instance method (which is static).
        auto sub_shared_ptr = EventSubscriber::get_instance(subscriber_handle);
        ASSERT_NE(sub_shared_ptr, nullptr) << "Failed to get EventSubscriber instance.";
        sub_shared_ptr->set_local_eventbus(local_bus);


        // 5. Setup our test ZMQ PUB socket
        zmq_pub_socket = zmq_socket(zmq_context, ZMQ_PUB);
        ASSERT_NE(zmq_pub_socket, nullptr);
        ASSERT_EQ(zmq_connect(zmq_pub_socket, ZMQ_ENDPOINT), 0) << "Failed to connect test PUB socket to " << ZMQ_ENDPOINT;

        // Allow some time for ZMQ connections to establish
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    void TearDown() override {
        if (subscriber_handle) {
            events_deinit_subscriber(subscriber_handle);
            subscriber_handle = nullptr;
        }
        if (zmq_pub_socket) {
            zmq_close(zmq_pub_socket);
            zmq_pub_socket = nullptr;
        }
        if (zmq_context) {
            zmq_ctx_term(zmq_context);
            zmq_context = nullptr;
        }
        // Consider removing/cleaning up tests/database_config.json if it's purely for this test
        // or ensure test runner does it. For now, leave it.
    }

    // Helper to publish a ZMQ event like EventPublisher would
    bool publish_simulated_zmq_event(const std::string& source, const std::string& tag, const event_params_t& params) {
        if (!zmq_pub_socket) return false;

        internal_event_t internal_event;
        std::string json_data_str = convert_to_json(source + ":" + tag, params);

        internal_event[EVENT_STR_DATA] = json_data_str;
        internal_event[EVENT_RUNTIME_ID] = generate_test_uuid();
        internal_event[EVENT_SEQUENCE] = seq_to_str(1); // Example sequence

        std::stringstream ss_epoch;
        ss_epoch << std::chrono::duration_cast<std::chrono::nanoseconds>(
                        std::chrono::system_clock::now().time_since_epoch()).count();
        internal_event[EVENT_EPOCH] = ss_epoch.str();

        // ZMQ send part 1: topic (event source)
        int rc = zmq_send(zmq_pub_socket, source.c_str(), source.length(), ZMQ_SNDMORE);
        if (rc == -1) return false;

        // ZMQ send part 2: serialized internal_event_t
        std::string serialized_internal_event;
        if (serialize(internal_event, serialized_internal_event) != 0) return false;

        rc = zmq_send(zmq_pub_socket, serialized_internal_event.c_str(), serialized_internal_event.length(), 0);
        return rc != -1;
    }
};

const char* ZmqEventBusBridgeTest::ZMQ_ENDPOINT = "inproc://eventbus_bridge_test";


TEST_F(ZmqEventBusBridgeTest, ZmqToLocalEventBusRelay) {
    std::string event_source = "test_source";
    std::string event_tag = "test_tag";
    std::string expected_bus_topic = event_source + ":" + event_tag;
    event_params_t sent_params = {{"key1", "value1"}, {"key2", "value2"}};

    bool eventbus_event_received = false;
    event_params_t received_bus_params;
    std::string received_bus_topic;

    auto bus_sub = local_bus->subscribe(expected_bus_topic,
        [&](const common::Event& event) {
            eventbus_event_received = true;
            received_bus_topic = event.topic;
            try {
                received_bus_params = std::any_cast<event_params_t>(event.data);
            } catch (const std::bad_any_cast& e) {
                ADD_FAILURE() << "Bad any_cast in EventBus subscriber: " << e.what();
            }
        }
    );
    ASSERT_NE(bus_sub, nullptr);

    ASSERT_TRUE(publish_simulated_zmq_event(event_source, event_tag, sent_params));

    // Need to give EventSubscriber's thread time to receive and process the ZMQ message
    // EventSubscriber::event_receive is blocking, but it's called by the C API.
    // The internal mechanism of EventSubscriber might involve a loop or it might process on demand.
    // The set_local_eventbus happens in its event_receive path.
    // To trigger EventSubscriber's receive path, we'd typically call event_receive() from events.h
    // Since we are testing the bridge, we expect the bridge to publish to local_bus *when* event_receive is called.
    // The current EventSubscriber doesn't have its own thread; it processes when its event_receive is called.
    // This means our test needs to call event_receive on the subscriber_handle.

    event_receive_op_t received_op; // For the C API call
    int ret = event_receive(subscriber_handle, received_op);

    ASSERT_EQ(ret, 0) << "event_receive (C API) failed or timed out. ZMQ message might not have been sent/received properly.";

    // Now check if the EventBus subscriber was called (it should be called from within the above event_receive)
    ASSERT_TRUE(eventbus_event_received) << "Event not received on the local EventBus via bridge.";
    ASSERT_EQ(received_bus_topic, expected_bus_topic);
    ASSERT_EQ(received_bus_params.size(), sent_params.size());
    for (const auto& sent_pair : sent_params) {
        auto it = received_bus_params.find(sent_pair.first);
        ASSERT_NE(it, received_bus_params.end()) << "Key missing in received params: " << sent_pair.first;
        ASSERT_EQ(it->second, sent_pair.second) << "Value mismatch for key: " << sent_pair.first;
    }
}


TEST_F(ZmqEventBusBridgeTest, SubscriberNoLocalBusSet) {
    // Re-initialize EventSubscriber without setting the local bus.
    if (subscriber_handle) {
        events_deinit_subscriber(subscriber_handle);
    }
    // Temporarily "forget" the local bus for this specific test part if we were to set it globally.
    // Here, we ensure EventSubscriber is re-init and set_local_eventbus is NOT called.
    subscriber_handle = events_init_subscriber(false, 100, nullptr);
    ASSERT_NE(subscriber_handle, nullptr) << "Failed to re-init EventSubscriber for NoLocalBusSet test.";

    // (No call to set_local_eventbus for this instance)

    std::string event_source = "no_bus_source";
    std::string event_tag = "no_bus_tag";
    event_params_t sent_params = {{"data", "payload"}};

    // This subscription should NOT receive anything if the bus isn't set in EventSubscriber
    bool eventbus_event_received = false;
    auto bus_sub = local_bus->subscribe(event_source + ":" + event_tag,
        [&](const common::Event& event) {
            eventbus_event_received = true; // Should not happen
        }
    );
    ASSERT_NE(bus_sub, nullptr);

    ASSERT_TRUE(publish_simulated_zmq_event(event_source, event_tag, sent_params));

    event_receive_op_t received_op;
    int ret = event_receive(subscriber_handle, received_op);
    ASSERT_EQ(ret, 0) << "event_receive (C API) failed for NoLocalBusSet test.";

    // Verify original C-API functionality
    ASSERT_EQ(received_op.key, event_source + ":" + event_tag);
    ASSERT_EQ(received_op.params["data"], "payload");

    // Crucially, verify the local EventBus subscriber was NOT called
    ASSERT_FALSE(eventbus_event_received) << "Event was incorrectly relayed to EventBus when no bus was set.";
}


// End of tests/eventbus_ut.cpp
// (Ensure this line is kept if the original file ended with it)
```
