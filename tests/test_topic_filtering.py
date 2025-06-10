import time
import pytest
from swsscommon import swsscommon
import os

# Helper function to get a DBConnector instance for testing
# Assumes Redis is running. Uses ASIC_DB by default.
def get_db_connector():
    # In SONiC, database_config.json is typically preloaded.
    # For local testing, we might need to point to it or ensure Redis is accessible.
    # Using default constructor for DBConnector if it connects to a default local instance.
    # Some tests in swsscommon use specific DBs like 'ASIC_DB'.
    # The DBConnector(db_name, timeout, unixsocket) or DBConnector(db_name, host, port, timeout)
    # might be needed if a specific setup is required.
    # For now, let's assume 'ASIC_DB' is available or the default connector works.
    return swsscommon.DBConnector("ASIC_DB", 0, True) # Use unix socket, instantaneous timeout for commands

# Test cases for FilteredNotificationConsumer (Python bindings)
class TestFilteredNotificationConsumer:

    def test_specific_topics(self):
        db = get_db_connector()
        channel_name = "PY_FN_CHAN_ST" # Unique channel
        producer = swsscommon.NotificationProducer(db, channel_name)
        consumer = swsscommon.FilteredNotificationConsumer(db, channel_name)

        consumer.subscribeTopic("PY_TOPIC_A")
        consumer.subscribeTopic("PY_TOPIC_C")

        fvA = swsscommon.FieldValuePairs([
            (swsscommon.JSON_FIELD_ENTITY_ID, "PY_TOPIC_A"), ("value", "valA")
        ])
        fvB = swsscommon.FieldValuePairs([
            (swsscommon.JSON_FIELD_ENTITY_ID, "PY_TOPIC_B"), ("value", "valB")
        ])
        fvC = swsscommon.FieldValuePairs([
            (swsscommon.JSON_FIELD_ENTITY_ID, "PY_TOPIC_C"), ("value", "valC")
        ])

        producer.send("SET", "PY_KEY_A", fvA)
        producer.send("SET", "PY_KEY_B", fvB) # Should be filtered
        producer.send("SET", "PY_KEY_C", fvC)

        # Allow some time for messages to propagate through Redis Pub/Sub
        time.sleep(0.1)

        received_keys = []

        # Pop first message (A or C)
        try:
            op, data, values = consumer.pop(100) # 100ms timeout for pop
            received_keys.append(data)
            assert data in ["PY_KEY_A", "PY_KEY_C"]
            # Verify entity_id in values
            entity_id_val = ""
            for k, v in values:
                if k == swsscommon.JSON_FIELD_ENTITY_ID:
                    entity_id_val = v
                    break
            assert entity_id_val in ["PY_TOPIC_A", "PY_TOPIC_C"]

        except RuntimeError as e:
            pytest.fail(f"Pop failed unexpectedly for first message: {e}")

        # Pop second message (A or C, the other one)
        try:
            op, data, values = consumer.pop(100)
            received_keys.append(data)
            assert data in ["PY_KEY_A", "PY_KEY_C"]
            assert len(received_keys) == 2
            assert "PY_KEY_A" in received_keys
            assert "PY_KEY_C" in received_keys
        except RuntimeError as e:
            pytest.fail(f"Pop failed unexpectedly for second message: {e}")

        # Try to pop again, should timeout (throw RuntimeError)
        with pytest.raises(RuntimeError):
            consumer.pop(100) # Short timeout

    def test_no_topics_subscribed(self):
        db = get_db_connector()
        channel_name = "PY_FN_CHAN_NT"
        producer = swsscommon.NotificationProducer(db, channel_name)
        consumer = swsscommon.FilteredNotificationConsumer(db, channel_name)

        fvA = swsscommon.FieldValuePairs([
            (swsscommon.JSON_FIELD_ENTITY_ID, "PY_TOPIC_A_NT"), ("value", "valANT")
        ])
        fvB_no_entity = swsscommon.FieldValuePairs([("value", "valBNT_no_entity")])

        producer.send("SET", "PY_KEY_A_NT", fvA)
        producer.send("SET", "PY_KEY_B_NT", fvB_no_entity)
        time.sleep(0.1)

        received_count = 0
        # Expect both due to pass-through
        try:
            op, d, v = consumer.pop(100); received_count +=1
            assert d == "PY_KEY_A_NT"
            op, d, v = consumer.pop(100); received_count +=1
            assert d == "PY_KEY_B_NT"
        except RuntimeError:
            pytest.fail("Should have received two messages with no topics subscribed.")

        assert received_count == 2
        with pytest.raises(RuntimeError):
            consumer.pop(100)

    def test_unsubscribe(self):
        db = get_db_connector()
        channel_name = "PY_FN_CHAN_US"
        producer = swsscommon.NotificationProducer(db, channel_name)
        consumer = swsscommon.FilteredNotificationConsumer(db, channel_name)

        consumer.subscribeTopic("PY_TOPIC_A_US")
        consumer.subscribeTopic("PY_TOPIC_B_US")

        fvA = swsscommon.FieldValuePairs([(swsscommon.JSON_FIELD_ENTITY_ID, "PY_TOPIC_A_US")])
        fvB = swsscommon.FieldValuePairs([(swsscommon.JSON_FIELD_ENTITY_ID, "PY_TOPIC_B_US")])

        producer.send("OP", "KEY_A1_US", fvA)
        producer.send("OP", "KEY_B1_US", fvB)
        time.sleep(0.1)

        # Pop A1 and B1
        op, d, v = consumer.pop(100); assert d in ["KEY_A1_US", "KEY_B1_US"]
        op, d, v = consumer.pop(100); assert d in ["KEY_A1_US", "KEY_B1_US"]

        consumer.unsubscribeTopic("PY_TOPIC_A_US")
        producer.send("OP", "KEY_A2_US", fvA) # Should be filtered
        producer.send("OP", "KEY_B2_US", fvB) # Should be received
        time.sleep(0.1)

        op, d, v = consumer.pop(100)
        assert d == "KEY_B2_US"

        with pytest.raises(RuntimeError):
            consumer.pop(100)


# Test cases for TopicEventSubscriber (Python bindings)
# These are more challenging to make fully self-contained without a mock event source
# or a real event publisher in the test environment.
# We will test the subscription management and that receive can be called.
# Actual event content filtering testing is better done via C++ UTs or integration tests.

class TestTopicEventSubscriber:

    def test_subscription_management_py(self):
        # Constructor params: event_source, use_cache, recv_timeout, sources_list
        # For this test, we are primarily checking Python bindings for subscribe/unsubscribe
        # The underlying events_init_subscriber might not succeed if no broker,
        # but the TopicEventSubscriber object itself should be creatable.
        try:
            subscriber = swsscommon.TopicEventSubscriber("PY_TES_SOURCE", False, 0, None)
        except Exception as e:
            pytest.fail(f"Failed to instantiate TopicEventSubscriber: {e}")

        assert not subscriber.isTopicSubscribed("PY_TOPIC_X")
        subscriber.subscribeTopic("PY_TOPIC_X")
        assert subscriber.isTopicSubscribed("PY_TOPIC_X")
        subscriber.unsubscribeTopic("PY_TOPIC_X")
        assert not subscriber.isTopicSubscribed("PY_TOPIC_X")

    def test_receive_call_py(self):
        # This test mainly ensures the receive method can be called.
        # It will likely timeout if no actual event system is running and connected.
        # The event_source "PY_TES_RECEIVE" is arbitrary for this test.
        subscriber = swsscommon.TopicEventSubscriber("PY_TES_RECEIVE", False, 10, None) # 10ms timeout
        subscriber.subscribeTopic("ANYTOPIC")

        evt_op = swsscommon.event_receive_op_t()
        try:
            return_code = subscriber.receive(evt_op, 50) # Override timeout to 50ms
            # Expected to timeout, so return_code > 0 (typically 1 for ZMQ_ETIMEDOUT)
            # or could be another positive value if mapped differently by event_receive
            assert return_code > 0
        except Exception as e:
            pytest.fail(f"subscriber.receive() call failed: {e}")
