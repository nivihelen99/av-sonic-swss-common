import unittest
import time
from swsscommon import swsscommon

# Helper to parse concatenated fields string "f1:v1,f2:v2" into a dict
def parse_fields_str_to_dict(fields_str):
    if not fields_str:
        return {}
    fields = {}
    pairs = fields_str.split(',')
    for pair in pairs:
        parts = pair.split(':', 1)
        if len(parts) == 2:
            fields[parts[0]] = parts[1]
        elif len(parts) == 1: # Field with empty value
            fields[parts[0]] = ""
    return fields

class TestStream(unittest.TestCase):
    DBCONFIG_PATH = "/var/run/redis/sonic-db/database_config.json"
    APP_DB_ID = 0
    TEST_STREAM_PREFIX = "PY_STREAM_UT_"
    _stream_counter = 0

    @classmethod
    def setUpClass(cls):
        # It's possible that loading DB config is needed if not done globally
        # For now, assume DBConnector connections will work if Redis is at localhost:6379
        try:
            swsscommon.SonicDBConfig.load_sonic_db_config(cls.DBCONFIG_PATH)
        except Exception as e:
            print(f"Failed to load SonicDBConfig: {e}. Tests might fail if DB not accessible.")
            # Continue, as DBConnector can also be initialized with host/port directly.

        # DB connector for flushing DB
        cls.main_db = swsscommon.DBConnector(cls.APP_DB_ID, "127.0.0.1", 6379, 0)


    @classmethod
    def tearDownClass(cls):
        pass # No class-level cleanup needed beyond what tests do

    def setUp(self):
        # Flush APP_DB before each test
        self.main_db.flushdb()
        self.db = swsscommon.DBConnector(self.APP_DB_ID, "127.0.0.1", 6379, 0)
        TestStream._stream_counter += 1
        self.stream_name_base = f"{self.TEST_STREAM_PREFIX}{self._testMethodName}_{TestStream._stream_counter}"

    def tearDown(self):
        # Optional: Could delete test streams here, but flushdb in setUp handles it.
        del self.db

    def get_unique_stream_name(self, suffix=""):
        return f"{self.stream_name_base}{suffix}"

    def test_xadd_xlen(self):
        stream_name = self.get_unique_stream_name()
        s = swsscommon.Stream(self.db, stream_name)

        self.assertEqual(s.xlen(), 0)

        id1 = s.xadd("*", [("field1", "value1"), ("field2", "value2")])
        self.assertTrue(id1)
        self.assertNotEqual(id1, "0-0")
        self.assertEqual(s.xlen(), 1)

        id2 = s.xadd("*", [("temp", "hot")])
        self.assertTrue(id2)
        self.assertNotEqual(id2, id1)
        self.assertEqual(s.xlen(), 2)

        # Specific ID (must be greater)
        ts, seq = map(int, id2.split('-'))
        specific_id = f"{ts}-{seq + 1}"
        id3 = s.xadd(specific_id, [("specific", "id_test")])
        self.assertEqual(id3, specific_id)
        self.assertEqual(s.xlen(), 3)
    
    def test_xtrim_maxlen(self):
        stream_name = self.get_unique_stream_name()
        s = swsscommon.Stream(self.db, stream_name)

        s.xadd("*", [("f", "1")])
        s.xadd("*", [("f", "2")])
        id3_added = s.xadd("*", [("f", "3")])
        s.xadd("*", [("f", "4")])
        s.xadd("*", [("f", "5")])
        self.assertEqual(s.xlen(), 5)

        trimmed_count = s.xtrim("MAXLEN", "3", False) # approximate=False
        self.assertEqual(trimmed_count, 2)
        self.assertEqual(s.xlen(), 3)

        # Verify remaining elements
        # Expected Python type for range_res: StreamReadResult
        # StreamReadResult should be iterable (like a list)
        # Each item: StreamMessagesEntry (std::pair<std::string, StreamMessageList>)
        # StreamMessageList: std::vector<StreamMessageTuple> (std::vector<std::pair<std::string, std::string>>)
        # StreamMessageTuple: (id_str, fields_str)
        range_res_ptr = s.xrange("-", "+")
        self.assertIsNotNone(range_res_ptr)
        range_res = list(range_res_ptr.get_data()) # Assuming get_data() or similar from shared_ptr wrapper
        
        self.assertEqual(len(range_res), 1)
        self.assertEqual(range_res[0][0], stream_name) # stream_name_str
        messages = range_res[0][1] # list of (id_str, fields_str)
        self.assertEqual(len(messages), 3)
        self.assertEqual(messages[0][0], id3_added) # id of first remaining message
        self.assertEqual(parse_fields_str_to_dict(messages[0][1]), {"f": "3"})

        s.xadd("*", [("f", "6")])
        s.xadd("*", [("f", "7")]) # Now 5 elements
        trimmed_count = s.xtrim("MAXLEN", "2", True) # approximate=True
        self.assertGreaterEqual(trimmed_count, 3)
        self.assertLessEqual(s.xlen(), 2)

        s.xadd("*", [("f", "8")]) # Add one more to non-empty
        prev_len = s.xlen()
        trimmed_count = s.xtrim("MAXLEN", "0") # Trim all
        self.assertEqual(trimmed_count, prev_len)
        self.assertEqual(s.xlen(), 0)

    def test_xtrim_minid(self):
        stream_name = self.get_unique_stream_name()
        s = swsscommon.Stream(self.db, stream_name)

        s.xadd("1000-0", [("f", "1")])
        s.xadd("1000-1", [("f", "2")])
        id3_expected = s.xadd("2000-0", [("f", "3")])
        s.xadd("2000-5", [("f", "4")])
        s.xadd("3000-0", [("f", "5")])
        self.assertEqual(s.xlen(), 5)

        trimmed_count = s.xtrim("MINID", "2000-0") # default approximate=False
        self.assertEqual(trimmed_count, 2)
        self.assertEqual(s.xlen(), 3)
        
        range_res_ptr = s.xrange("-", "+")
        range_res = list(range_res_ptr.get_data())
        self.assertEqual(len(range_res[0][1]), 3)
        self.assertEqual(range_res[0][1][0][0], id3_expected)

        s.xadd("1999-10", [("f", "0")])
        self.assertEqual(s.xlen(), 4)
        trimmed_count = s.xtrim("MINID", "2000-0", True) # approximate=True
        self.assertEqual(trimmed_count, 1)
        self.assertEqual(s.xlen(), 3)
        
        s.xadd("1000-0", [("f", "a")])
        s.xadd("1000-1", [("f", "b")])
        s.xadd("1000-2", [("f", "c")]) # 3 old, 3 new = 6 total
        trimmed_count = s.xtrim("MINID", "2000-0", False, 2) # limit = 2
        self.assertEqual(trimmed_count, 2)
        self.assertEqual(s.xlen(), 4)
        range_res_ptr = s.xrange("-", "+")
        range_res = list(range_res_ptr.get_data())
        self.assertEqual(parse_fields_str_to_dict(range_res[0][1][0][1]), {"f": "c"}) # 1000-2 should be first among 1000-x

    def test_xdel(self):
        stream_name = self.get_unique_stream_name()
        s = swsscommon.Stream(self.db, stream_name)

        id1 = s.xadd("*", [("f", "1")])
        id2 = s.xadd("*", [("f", "2")])
        id3 = s.xadd("*", [("f", "3")])
        self.assertEqual(s.xlen(), 3)

        del_count = s.xdel([id2])
        self.assertEqual(del_count, 1)
        self.assertEqual(s.xlen(), 2)

        range_res_ptr = s.xrange("-", "+")
        range_res = list(range_res_ptr.get_data())[0][1]
        self.assertEqual(len(range_res), 2)
        self.assertEqual(range_res[0][0], id1)
        self.assertEqual(range_res[1][0], id3)
        
        id4 = s.xadd("*", [("f", "4")])
        id5 = s.xadd("*", [("f", "5")]) # 4 elements: id1, id3, id4, id5
        del_count = s.xdel([id1, id3, "non-existent-id"])
        self.assertEqual(del_count, 2)
        self.assertEqual(s.xlen(), 2)

        del_count = s.xdel([id4, id5])
        self.assertEqual(del_count, 2)
        self.assertEqual(s.xlen(), 0)
        self.assertEqual(s.xdel(["some-id"]), 0)

    def test_xrange_xrevrange(self):
        stream_name = self.get_unique_stream_name()
        s = swsscommon.Stream(self.db, stream_name)

        id1 = s.xadd("1000-0", [("f", "1")])
        id2 = s.xadd("1000-1", [("f", "2")])
        id3 = s.xadd("2000-0", [("f", "3")])
        id4 = s.xadd("2000-1", [("f", "4")])
        id5 = s.xadd("3000-0", [("f", "5")])

        res_ptr = s.xrange("-", "+")
        res = list(res_ptr.get_data())[0][1]
        self.assertEqual(len(res), 5)
        self.assertEqual(res[0][0], id1)
        self.assertEqual(parse_fields_str_to_dict(res[4][1])["f"], "5")

        res_ptr = s.xrange(id2, id4, -1) # count = -1 (all in range)
        res = list(res_ptr.get_data())[0][1]
        self.assertEqual(len(res), 3)
        self.assertEqual(res[0][0], id2)
        self.assertEqual(res[2][0], id4)

        res_ptr = s.xrange("-", "+", 2) # count = 2
        res = list(res_ptr.get_data())[0][1]
        self.assertEqual(len(res), 2)
        self.assertEqual(res[0][0], id1)

        # XREVRANGE
        res_ptr = s.xrevrange("+", "-")
        res = list(res_ptr.get_data())[0][1]
        self.assertEqual(len(res), 5)
        self.assertEqual(res[0][0], id5)
        self.assertEqual(parse_fields_str_to_dict(res[4][1])["f"], "1")

        res_ptr = s.xrevrange(id4, id2, -1)
        res = list(res_ptr.get_data())[0][1]
        self.assertEqual(len(res), 3)
        self.assertEqual(res[0][0], id4)
        self.assertEqual(res[2][0], id2)

        res_ptr = s.xrevrange("+", "-", 2)
        res = list(res_ptr.get_data())[0][1]
        self.assertEqual(len(res), 2)
        self.assertEqual(res[0][0], id5)
    
    def test_xread_single_stream(self):
        stream_name = self.get_unique_stream_name()
        s = swsscommon.Stream(self.db, stream_name)
        
        # XREAD from empty stream (non-blocking)
        res_ptr = s.xread(["0-0"], 1, 0) # ids, count, block_ms
        self.assertIsNone(res_ptr) # Expect None if no data (DBConnector returns nullptr)

        r_id1 = s.xadd("*", [("f", "a")])
        s.xadd("*", [("f", "b")])

        res_ptr = s.xread(["0-0"], 1) # Read 1 message
        self.assertIsNotNone(res_ptr)
        res = list(res_ptr.get_data())
        self.assertEqual(len(res), 1)
        self.assertEqual(res[0][0], stream_name)
        messages = res[0][1]
        self.assertEqual(len(messages), 1)
        self.assertEqual(messages[0][0], r_id1)

        # XREAD with BLOCK (short timeout)
        last_id = s.xadd("*", [("f", "c")])
        res_ptr = s.xread([last_id], 1, 10) # Block for 10ms
        self.assertIsNone(res_ptr) # Should timeout

    def test_xgroup_management(self):
        stream_name = self.get_unique_stream_name()
        s = swsscommon.Stream(self.db, stream_name)
        group_name = "mygroup"

        self.assertFalse(s.xgroup_create(group_name, "0-0", False)) # mkstream=False, stream not exist
        self.assertTrue(s.xgroup_create(group_name, "0-0", True))  # mkstream=True
        self.assertEqual(s.xlen(), 0)

        s.xadd("*", [("f", "1")])
        self.assertFalse(s.xgroup_create(group_name, "0-0", False)) # Group already exists

        self.assertEqual(s.xgroup_delconsumer(group_name, "consumer1"), 0) # Consumer not exist
        self.assertTrue(s.xgroup_destroy(group_name))
        self.assertFalse(s.xgroup_destroy("non_existent_group"))

    def test_xreadgroup_ack_pending_claim(self):
        stream_name = self.get_unique_stream_name()
        s = swsscommon.Stream(self.db, stream_name)
        group = "grp1"
        c1 = "consumer1"
        c2 = "consumer2"

        self.assertTrue(s.xgroup_create(group, "0-0", True))

        id1 = s.xadd("*", [("order", "apple")])
        id2 = s.xadd("*", [("order", "banana")])
        s.xadd("*", [("order", "cherry")])

        res_c1_ptr = s.xreadgroup(group, c1, ">", 2) # Read 2 messages
        self.assertIsNotNone(res_c1_ptr)
        res_c1 = list(res_c1_ptr.get_data())[0][1]
        self.assertEqual(len(res_c1), 2)
        self.assertEqual(res_c1[0][0], id1)
        self.assertEqual(res_c1[1][0], id2)

        # XPENDING - requires parsing RedisReply object
        # The Python binding for Stream::xpending returns a RedisReply shared_ptr.
        # We need to call methods on this RedisReply object.
        pending_reply = s.xpending(group) # Default args: "-", "+", -1, ""
        self.assertIsNotNone(pending_reply)
        self.assertEqual(pending_reply.get_type_str(), "ARRAY")
        # Summary: [count, min_id, max_id, [consumer_info_array]]
        elements = pending_reply.get_elements()
        self.assertGreaterEqual(len(elements), 3)
        self.assertEqual(elements[0].get_integer(), 2) # 2 pending
        self.assertEqual(elements[1].get_string(), id1)
        self.assertEqual(elements[2].get_string(), id2)
        # Deeper consumer check if present
        if len(elements) > 3 and elements[3].get_type_str() == "ARRAY":
            consumer_details = elements[3].get_elements()
            self.assertEqual(len(consumer_details), 1) # c1
            self.assertEqual(consumer_details[0].get_elements()[0].get_string(), c1)
            self.assertEqual(consumer_details[0].get_elements()[1].get_string(), "2") # pending count for c1


        self.assertEqual(s.xack(group, [id1]), 1)
        pending_reply = s.xpending(group)
        self.assertEqual(pending_reply.get_elements()[0].get_integer(), 1) # 1 pending (id2)

        # C2 reads (should get id3)
        res_c2_ptr = s.xreadgroup(group, c2, ">", 2)
        res_c2 = list(res_c2_ptr.get_data())[0][1]
        self.assertEqual(len(res_c2), 1)
        id3_read_by_c2 = res_c2[0][0]
        self.assertNotEqual(id3_read_by_c2, id1)
        self.assertNotEqual(id3_read_by_c2, id2)


        # XCLAIM id2 by C2
        time.sleep(0.01) # Ensure some idle time
        claimed_ptr = s.xclaim(group, c2, 0, [id2], False) # min_idle_time=0, justid=False
        self.assertIsNotNone(claimed_ptr)
        claimed_res = list(claimed_ptr.get_data())[0][1]
        self.assertEqual(len(claimed_res), 1)
        self.assertEqual(claimed_res[0][0], id2)
        self.assertEqual(parse_fields_str_to_dict(claimed_res[0][1])["order"], "banana")

        # XPENDING detailed for c2
        pending_c2_reply = s.xpending(group, "-", "+", 10, c2)
        self.assertIsNotNone(pending_c2_reply)
        self.assertEqual(pending_c2_reply.get_type_str(), "ARRAY")
        # Detailed: [[id, consumer, idle_ms, delivery_count], ...]
        detailed_elements = pending_c2_reply.get_elements()
        found_id2_for_c2 = any(entry.get_elements()[0].get_string() == id2 and \
                               entry.get_elements()[1].get_string() == c2 \
                               for entry in detailed_elements)
        self.assertTrue(found_id2_for_c2)

        self.assertEqual(s.xack(group, [id2, id3_read_by_c2]), 2)
        pending_reply = s.xpending(group)
        self.assertEqual(pending_reply.get_elements()[0].get_integer(), 0)

        # XREADGROUP with NOACK
        id4_noack = s.xadd("*", {("ack", "no")})
        res_c1_noack_ptr = s.xreadgroup(group, c1, ">", 1, True, -1) # noack=True
        self.assertIsNotNone(res_c1_noack_ptr)
        res_c1_noack = list(res_c1_noack_ptr.get_data())[0][1]
        self.assertEqual(res_c1_noack[0][0], id4_noack)
        
        pending_reply = s.xpending(group) # Should still be 0
        self.assertEqual(pending_reply.get_elements()[0].get_integer(), 0)

        # XCLAIM JUSTID
        id_to_claim = s.xadd("*", {("k", "v")})
        s.xreadgroup(group, c1, ">", 1) # Read by c1
        time.sleep(0.01)
        claimed_justid_ptr = s.xclaim(group, c2, 0, [id_to_claim], True) # justid=True
        self.assertIsNotNone(claimed_justid_ptr)
        claimed_justid_res = list(claimed_justid_ptr.get_data())[0][1]
        self.assertEqual(len(claimed_justid_res), 1)
        self.assertEqual(claimed_justid_res[0][0], id_to_claim)
        self.assertTrue(not claimed_justid_res[0][1] or claimed_justid_res[0][1] == "") # Fields empty


if __name__ == '__main__':
    unittest.main()
