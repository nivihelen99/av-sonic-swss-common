# Redis Streams Support in SWSS

This document describes the Redis Streams functionality integrated into the SWSS (Switch State Service) common library (`libswsscommon`). It covers the design, C++ API, C API, Python API, and testing strategy.

## Design Overview

Redis Streams are a powerful data structure in Redis that allows for append-only, persistent message queues. Support for Redis Streams has been added to `libswsscommon` to enable services to leverage this feature for inter-process communication, event logging, and other scenarios requiring ordered message handling.

The integration primarily consists of two layers:

1.  **`DBConnector` Extensions**: The low-level `swss::DBConnector` class has been extended with new methods that directly map to Redis Streams commands (e.g., `xadd`, `xread`, `xgroupcreate`). These methods provide raw access to stream functionalities and are responsible for constructing and executing commands via `hiredis`. They handle parsing of Redis replies into C++ data structures (often `std::shared_ptr` to complex nested vectors and pairs).

2.  **`Stream` Class**: A new C++ class, `swss::Stream`, provides a higher-level abstraction for interacting with a specific Redis Stream. It encapsulates a `DBConnector` instance and a stream name, offering methods that call the underlying `DBConnector` stream commands. This class simplifies stream operations by managing the stream context.

This layered approach offers both direct control via `DBConnector` and ease of use via the `Stream` class.

## C++ API

The C++ API for Redis Streams is available through `DBConnector` and the `Stream` class.

### `DBConnector` Stream Methods

The following methods have been added to `swss::DBConnector` in `common/dbconnector.h`:

*   `std::string xadd(const std::string &key, const std::string &id, const std::vector<std::pair<std::string, std::string>> &values, int maxlen = 0, bool approximate = false)`: Adds an entry to the stream `key` with the specified `id` (e.g., "*" for auto-generated) and field-value pairs. `maxlen` and `approximate` control stream trimming. Returns the ID of the added entry.
*   `std::shared_ptr<std::vector<std::pair<std::string, std::vector<std::pair<std::string, std::string>>>>> xread(const std::vector<std::string> &keys, const std::vector<std::string> &ids, int count = -1, int block_ms = -1)`: Reads messages from one or more streams specified in `keys`, starting after the corresponding `ids`. `count` limits messages per stream. `block_ms` specifies blocking timeout (-1 for no block, 0 for indefinite). Returns a shared pointer to a vector of pairs; each pair contains a stream name and a vector of messages (ID and concatenated fields string). Returns `nullptr` on timeout or if no new messages.
*   `bool xgroup_create(const std::string &key, const std::string &groupname, const std::string &id = "$", bool mkstream = false)`: Creates a consumer group `groupname` for stream `key` starting from `id`. If `mkstream` is true, creates the stream if it doesn't exist. Returns `true` on success.
*   `bool xgroup_destroy(const std::string &key, const std::string &groupname)`: Destroys a consumer group. Returns `true` if successful.
*   `int64_t xgroup_delconsumer(const std::string &key, const std::string &groupname, const std::string &consumername)`: Deletes a consumer from a group. Returns the number of pending messages the consumer had before deletion.
*   `std::shared_ptr<std::vector<std::pair<std::string, std::vector<std::pair<std::string, std::string>>>>> xreadgroup(const std::string &groupname, const std::string &consumername, const std::vector<std::string> &keys, const std::vector<std::string> &ids, int count = -1, bool noack = false, int block_ms = -1)`: Reads messages for `consumername` in `groupname` from specified `keys` starting from `ids` (e.g., ">" for new messages). `noack` controls if messages are added to PEL. Return type similar to `xread`.
*   `int64_t xack(const std::string &key, const std::string &groupname, const std::vector<std::string> &ids)`: Acknowledges messages, removing them from the Pending Entries List (PEL). Returns the number of messages successfully acknowledged.
*   `std::shared_ptr<RedisReply> xpending(const std::string &key, const std::string &groupname, const std::string &start = "-", const std::string &end = "+", int count = -1, const std::string &consumername = "")`: Inspects pending messages. Returns a `RedisReply` object that needs to be parsed based on whether it's a summary or detailed view.
*   `std::shared_ptr<std::vector<std::pair<std::string, std::vector<std::pair<std::string, std::string>>>>> xclaim(const std::string &key, const std::string &groupname, const std::string &consumername, int min_idle_time, const std::vector<std::string> &ids, bool justid = false)`: Claims ownership of pending messages. `min_idle_time` specifies how long a message must be idle. `justid` returns only IDs. Return type similar to `xread`.
*   `int64_t xlen(const std::string &key)`: Returns the length of the stream.
*   `int64_t xtrim(const std::string &key, const std::string &strategy, const std::string &threshold, bool approximate = false, int limit = 0)`: Trims the stream using `MAXLEN` or `MINID` strategies. `threshold` is the length or ID. `approximate` enables faster but less precise trimming. `limit` restricts the number of entries actually deleted in a single XTRIM command when strategy is MINID. Returns the number of entries deleted.
*   `std::shared_ptr<std::vector<std::pair<std::string, std::vector<std::pair<std::string, std::string>>>>> xrange(const std::string &key, const std::string &start, const std::string &end, int count = -1)`: Returns messages within an ID range. Return type similar to `xread`.
*   `std::shared_ptr<std::vector<std::pair<std::string, std::vector<std::pair<std::string, std::string>>>>> xrevrange(const std::string &key, const std::string &end, const std::string &start, int count = -1)`: Returns messages in reverse ID range. Return type similar to `xread`.
*   `int64_t xdel(const std::string &key, const std::vector<std::string> &ids)`: Deletes messages by ID. Returns the number of messages deleted.

**Return Type for Read Operations (`xread`, `xreadgroup`, `xclaim`, `xrange`, `xrevrange`):**
These methods return a `std::shared_ptr` to a data structure:
`std::vector<std::pair<std::string, std::vector<std::pair<std::string, std::string>>>>`
This can be interpreted as:
- A vector, where each element corresponds to a stream that had messages.
    - Each element is a pair:
        - `first` (std::string): The name of the stream.
        - `second` (std::vector): A list of messages from that stream.
            - Each message is a pair:
                - `first` (std::string): The message ID.
                - `second` (std::string): A string concatenating all field-value pairs of the message, e.g., `"field1:value1,field2:value2"`.

### `swss::Stream` Class

The `swss::Stream` class (`common/stream.h`) provides a more object-oriented way to interact with a single Redis Stream.

**Constructor:**
`Stream(DBConnector *db, const std::string &streamName)`
- `db`: A pointer to an initialized `DBConnector` instance (e.g., connected to `APP_DB`).
- `streamName`: The name of the Redis Stream this object will interact with.

**Public Methods:**
The `Stream` class methods largely mirror the `DBConnector` stream methods but are called on a `Stream` object, so the stream name (`key`) parameter is implicit.

*   `std::string xadd(const std::string &id, const std::vector<std::pair<std::string, std::string>> &values, int maxlen = 0, bool approximate = false)`
*   `std::shared_ptr<std::vector<std::pair<std::string, std::vector<std::pair<std::string, std::string>>>>> xread(const std::vector<std::string> &ids, int count = -1, int block_ms = -1)`: Reads from the stream associated with the `Stream` object. `ids` is a vector containing a single ID for this stream.
*   `std::shared_ptr<std::vector<std::pair<std::string, std::vector<std::pair<std::string, std::string>>>>> xread(const std::vector<std::string> &keys, const std::vector<std::string> &ids, int count = -1, int block_ms = -1)`: (Less common for `Stream` object) Allows reading from multiple arbitrary `keys`.
*   `bool xgroup_create(const std::string &groupname, const std::string &id = "$", bool mkstream = false)`
*   `bool xgroup_destroy(const std::string &groupname)`
*   `int64_t xgroup_delconsumer(const std::string &groupname, const std::string &consumername)`
*   `std::shared_ptr<std::vector<std::pair<std::string, std::vector<std::pair<std::string, std::string>>>>> xreadgroup(const std::string &groupname, const std::string &consumername, const std::vector<std::string> &ids, int count = -1, bool noack = false, int block_ms = -1)`: Reads for the `Stream`'s stream. `ids` is typically `std::vector<std::string> {">"}`.
*   `std::shared_ptr<std::vector<std::pair<std::string, std::vector<std::pair<std::string, std::string>>>>> xreadgroup(const std::string &groupname, const std::string &consumername, const std::string &id = ">", int count = -1, bool noack = false, int block_ms = -1)`: Overload for single ID.
*   `int64_t xack(const std::string &groupname, const std::vector<std::string> &ids)`
*   `std::shared_ptr<RedisReply> xpending(const std::string &groupname, const std::string &start = "-", const std::string &end = "+", int count = -1, const std::string &consumername = "")`
*   `std::shared_ptr<std::vector<std::pair<std::string, std::vector<std::pair<std::string, std::string>>>>> xclaim(const std::string &groupname, const std::string &consumername, int min_idle_time, const std::vector<std::string> &ids, bool justid = false)`
*   `int64_t xlen()`
*   `int64_t xtrim(const std::string &strategy, const std::string &threshold, bool approximate = false, int limit = 0)`
*   `std::shared_ptr<std::vector<std::pair<std::string, std::vector<std::pair<std::string, std::string>>>>> xrange(const std::string &start, const std::string &end, int count = -1)`
*   `std::shared_ptr<std::vector<std::pair<std::string, std::vector<std::pair<std::string, std::string>>>>> xrevrange(const std::string &end, const std::string &start, int count = -1)`
*   `int64_t xdel(const std::vector<std::string> &ids)`
*   `const std::string& getName() const`: Returns the stream name.

### C++ Code Examples

```cpp
#include "dbconnector.h"
#include "stream.h"
#include "redisreply.h" // For parsing xpending results
#include <iostream>
#include <vector>
#include <string>

using namespace swss;

// Helper to parse concatenated fields string "f1:v1,f2:v2" into a map or print
void print_message_fields(const std::string& fields_str) {
    std::cout << "    Fields: ";
    // Simple split for example, real parsing might be more robust
    std::string current_field;
    size_t start = 0;
    size_t comma_pos = fields_str.find(',');
    while(comma_pos != std::string::npos) {
        current_field = fields_str.substr(start, comma_pos - start);
        std::cout << current_field << "; ";
        start = comma_pos + 1;
        comma_pos = fields_str.find(',', start);
    }
    current_field = fields_str.substr(start); // last field
    std::cout << current_field << std::endl;
}

int main() {
    // 1. Connecting and Instantiating Stream
    DBConnector db(APP_DB, "127.0.0.1", 6379, 0); // Assuming APP_DB is an int, e.g., 0
    std::string stream_key = "mystream_cpp_example";
    Stream data_stream(&db, stream_key);

    // 2. Adding messages using xadd
    std::vector<std::pair<std::string, std::string>> values1 = {{"sensor_id", "T1"}, {"temperature", "25.5"}};
    std::string id1 = data_stream.xadd("*", values1);
    std::cout << "Added message with ID: " << id1 << std::endl;

    std::vector<std::pair<std::string, std::string>> values2 = {{"sensor_id", "H1"}, {"humidity", "60.2"}};
    std::string id2 = data_stream.xadd("*", values2, 10, true); // MAXLEN ~10
    std::cout << "Added message with ID: " << id2 << " (with MAXLEN ~10)" << std::endl;

    std::cout << "Current stream length: " << data_stream.xlen() << std::endl;

    // 3. Reading messages using xrange
    std::cout << "\nReading with XRANGE (- to +):" << std::endl;
    auto range_result = data_stream.xrange("-", "+");
    if (range_result) {
        for (const auto& stream_data : *range_result) { // Should be one entry for data_stream.getName()
            std::cout << "Stream: " << stream_data.first << std::endl;
            for (const auto& message : stream_data.second) {
                std::cout << "  ID: " << message.first << std::endl;
                print_message_fields(message.second);
            }
        }
    }

    // 4. Creating a consumer group and reading with xreadgroup
    std::string group_name = "mygroup_cpp";
    std::string consumer_name = "consumer_cpp_1";
    if (data_stream.xgroup_create(group_name, "0-0", true)) { // mkstream=true (though stream exists)
        std::cout << "\nCreated consumer group: " << group_name << std::endl;
    } else {
        std::cout << "\nConsumer group " << group_name << " likely already exists." << std::endl;
    }
    
    std::cout << "\nReading with XREADGROUP (consumer: " << consumer_name << "):" << std::endl;
    // Read up to 2 new messages for this consumer
    auto group_read_result = data_stream.xreadgroup(group_name, consumer_name, ">", 2);
    std::vector<std::string> ack_ids;
    if (group_read_result) {
        for (const auto& stream_data : *group_read_result) {
            std::cout << "Stream: " << stream_data.first << std::endl;
            if (stream_data.second.empty()) {
                std::cout << "  No new messages for consumer " << consumer_name << std::endl;
            }
            for (const auto& message : stream_data.second) {
                std::cout << "  ID: " << message.first << " (read by " << consumer_name << ")" << std::endl;
                print_message_fields(message.second);
                ack_ids.push_back(message.first);
            }
        }
    } else {
         std::cout << "  No new messages for consumer " << consumer_name << std::endl;
    }

    // 5. Acknowledging messages with xack
    if (!ack_ids.empty()) {
        int64_t ack_count = data_stream.xack(group_name, ack_ids);
        std::cout << "\nAcknowledged " << ack_count << " messages." << std::endl;
    }
    
    // Clean up (optional, as test DBs are often flushed)
    // db.del(stream_key); // DBConnector::del for the key itself
    // data_stream.xgroup_destroy(group_name); // If you want to remove the group

    return 0;
}
```

## C API

A C API is provided for interacting with Redis Streams from C code. The API is defined in `common/c-api/stream.h`.

**Key Data Structures:**

*   `swss_stream_t`: An opaque pointer representing a Stream object instance.
*   `swss_stream_field_value_pair_t`: `{ const char *field; const char *value; }` for XADD input.
*   `swss_stream_message_entry_t`: `{ char *message_id; char *fields_concatenated; }`. Represents a single stream message. `fields_concatenated` is a string like `"field1:value1,field2:value2"`.
*   `swss_stream_messages_t`: `{ char *stream_name; swss_stream_message_entry_t *messages; size_t message_count; }`. Represents messages read from a stream.
*   `swss_stream_pending_summary_t`: For basic XPENDING reply (count, min/max ID).
*   `swss_stream_pending_entry_t`: For detailed XPENDING entries (ID, consumer, idle time, delivery count).

**Memory Management:**
Functions returning pointers to strings (e.g., `actual_message_id` in `swss_stream_xadd`) or structures containing them (e.g., `swss_stream_messages_t`) allocate memory. The caller is responsible for freeing this memory using the provided functions:
*   `swss_stream_free_string(char *str)`
*   `swss_stream_free_message_entries(swss_stream_message_entry_t *entries, size_t count)`
*   `swss_stream_free_messages_results(swss_stream_messages_t *results, size_t count)`
*   `swss_stream_free_pending_summary(swss_stream_pending_summary_t *summary)`
*   `swss_stream_free_pending_entries(swss_stream_pending_entry_t *entries, size_t count)`

All API functions return a `swss_rc_t` status code (e.g., `SWSS_RC_SUCCESS`, `SWSS_RC_INVALID_PARAM`).

**Core C API Functions:**

*   `swss_stream_create(swss_dbconnector_t *db, const char *stream_name, swss_stream_t **stream)`: Creates a stream handle.
*   `swss_stream_free(swss_stream_t *stream)`: Frees the stream handle.
*   `swss_stream_xadd(swss_stream_t *stream, const char *id_spec, const swss_stream_field_value_pair_t *fvs, size_t fv_count, int maxlen, bool approximate_trim, char **actual_message_id)`: Adds a message. `actual_message_id` is an output parameter.
*   `swss_stream_xread(swss_stream_t *stream, const char *const *ids, size_t id_count, int count_per_stream, int block_ms, swss_stream_messages_t **results, size_t *result_count)`: Reads messages. `id_count` typically 1 for this API. `results` and `result_count` are output.
*   `swss_stream_xgroup_create(swss_stream_t *stream, const char *group_name, const char *id_spec, bool mkstream, int *result_code)`: `result_code` is 0 for OK.
*   `swss_stream_xgroup_destroy(swss_stream_t *stream, const char *group_name, int *result_code)`: `result_code` is 1 for success.
*   `swss_stream_xgroup_delconsumer(swss_stream_t *stream, const char *group_name, const char *consumer_name, int64_t *count)`
*   `swss_stream_xreadgroup(swss_stream_t *stream, const char *group_name, const char *consumer_name, const char *const *ids, size_t id_count, int count_per_stream, bool noack, int block_ms, swss_stream_messages_t **results, size_t *result_count)`
*   `swss_stream_xack(swss_stream_t *stream, const char *group_name, const char *const *message_ids, size_t id_count, int64_t *acked_count)`
*   `swss_stream_xpending_summary(swss_stream_t *stream, const char *group_name, swss_stream_pending_summary_t *summary)`
*   `swss_stream_xpending_detailed(swss_stream_t *stream, const char *group_name, const char *start_id, const char *end_id, int count, const char *consumer_name, swss_stream_pending_entry_t **entries, size_t *entry_count)`
*   `swss_stream_xclaim(swss_stream_t *stream, const char *group_name, const char *consumer_name, int min_idle_time_ms, const char *const *message_ids, size_t id_count, bool justid, swss_stream_messages_t **results, size_t *result_count)`
*   `swss_stream_xlen(swss_stream_t *stream, int64_t *length)`
*   `swss_stream_xtrim(swss_stream_t *stream, const char *strategy, const char *threshold, bool approximate_trim, int limit, int64_t *trimmed_count)`
*   `swss_stream_xrange(swss_stream_t *stream, const char *start_id, const char *end_id, int count, swss_stream_messages_t **results, size_t *result_count)`
*   `swss_stream_xrevrange(swss_stream_t *stream, const char *end_id, const char *start_id, int count, swss_stream_messages_t **results, size_t *result_count)`
*   `swss_stream_xdel(swss_stream_t *stream, const char *const *message_ids, size_t id_count, int64_t *deleted_count)`

### C Code Examples

```c
#include "common/c-api/dbconnector.h" // For swss_dbconnector_t, swss_dbconnector_init, etc.
#include "common/c-api/stream.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// Assuming APP_DB_ID is defined (e.g., 0)
#define APP_DB_ID 0

void print_c_messages(swss_stream_messages_t *results, size_t result_count) {
    if (!results) {
        printf("  No messages.\n");
        return;
    }
    for (size_t i = 0; i < result_count; ++i) {
        printf("Stream: %s\n", results[i].stream_name);
        for (size_t j = 0; j < results[i].message_count; ++j) {
            printf("  ID: %s\n", results[i].messages[j].message_id);
            printf("    Fields: %s\n", results[i].messages[j].fields_concatenated);
        }
    }
}

int main() {
    swss_dbconnector_t *db = NULL;
    swss_rc_t rc = swss_dbconnector_init(APP_DB_ID, "127.0.0.1", 6379, 0, &db);
    if (rc != SWSS_RC_SUCCESS) {
        fprintf(stderr, "Failed to connect to DB\n");
        return EXIT_FAILURE;
    }

    const char *stream_name_c = "mystream_c_example";
    swss_stream_t *stream_c = NULL;
    rc = swss_stream_create(db, stream_name_c, &stream_c);
    if (rc != SWSS_RC_SUCCESS) {
        fprintf(stderr, "Failed to create stream handle\n");
        swss_dbconnector_free(db);
        return EXIT_FAILURE;
    }

    // XADD
    swss_stream_field_value_pair_t fvs[] = {{"sensor", "S1"}, {"value", "100"}};
    char *msg_id_c = NULL;
    rc = swss_stream_xadd(stream_c, "*", fvs, sizeof(fvs)/sizeof(fvs[0]), 0, false, &msg_id_c);
    if (rc == SWSS_RC_SUCCESS) {
        printf("Added message ID: %s\n", msg_id_c);
        swss_stream_free_string(msg_id_c);
    } else {
        fprintf(stderr, "XADD failed: %d\n", rc);
    }
    
    long long len;
    swss_stream_xlen(stream_c, &len);
    printf("Stream length: %lld\n", len);

    // XRANGE
    swss_stream_messages_t *range_results = NULL;
    size_t range_result_count = 0;
    printf("\nXRANGE results:\n");
    rc = swss_stream_xrange(stream_c, "-", "+", -1, &range_results, &range_result_count);
    if (rc == SWSS_RC_SUCCESS) {
        print_c_messages(range_results, range_result_count);
        swss_stream_free_messages_results(range_results, range_result_count);
    }

    // XGROUP CREATE & XREADGROUP
    const char *group_c = "mygroup_c";
    const char *consumer_c = "consumer_c_1";
    int group_rc;
    rc = swss_stream_xgroup_create(stream_c, group_c, "0-0", true, &group_rc);
    if (rc == SWSS_RC_SUCCESS && group_rc == 0) {
        printf("\nGroup %s created.\n", group_c);
    } else {
        printf("\nGroup %s creation failed or already exists.\n", group_c);
    }

    swss_stream_messages_t *group_read_results = NULL;
    size_t group_read_count = 0;
    const char *read_ids[] = {">"}; // For XREADGROUP, reading new messages for this stream
    printf("\nXREADGROUP results:\n");
    rc = swss_stream_xreadgroup(stream_c, group_c, consumer_c, read_ids, 1, 1, false, -1, 
                                &group_read_results, &group_read_count);
    if (rc == SWSS_RC_SUCCESS) {
        print_c_messages(group_read_results, group_read_count);
        if (group_read_count > 0 && group_read_results[0].message_count > 0) {
            // ACK messages
            const char *ack_ids_c[group_read_results[0].message_count];
            for(size_t i=0; i < group_read_results[0].message_count; ++i) {
                ack_ids_c[i] = group_read_results[0].messages[i].message_id;
            }
            long long acked_c_count;
            swss_stream_xack(stream_c, group_c, ack_ids_c, group_read_results[0].message_count, &acked_c_count);
            printf("\nAcknowledged %lld messages.\n", acked_c_count);
        }
        swss_stream_free_messages_results(group_read_results, group_read_count);
    }
    
    swss_stream_free(stream_c);
    swss_dbconnector_free(db);
    return EXIT_SUCCESS;
}
```

## Python API

Python bindings for the `Stream` class are provided via SWIG.

**Stream Class (`swsscommon.Stream`)**

*   **Constructor**: `Stream(db, streamName)`
    *   `db`: A `DBConnector` object.
    *   `streamName`: Name of the stream.

*   **Methods**: Methods mirror the C++ `swss::Stream` class.
    *   `xadd(id_spec, values, maxlen=0, approximate=False)`: `values` is a list of tuples, e.g., `[("field1", "value1"), ("field2", "value2")]`. Returns message ID string.
    *   `xread(ids, count=-1, block_ms=-1)`: `ids` is a list of strings (typically one ID for the stream). Returns a `StreamReadResult` object or `None`.
    *   `xreadKeys(keys, ids, count=-1, block_ms=-1)`: Similar to `xread` but takes a list of stream `keys` and corresponding `ids`. Returns `StreamReadResult` or `None`.
    *   `xgroupCreate(groupname, id="$", mkstream=False)`: Returns `True` on success.
    *   `xgroupDestroy(groupname)`: Returns `True` on success.
    *   `xgroupDelconsumer(groupname, consumername)`: Returns count of pending messages.
    *   `xreadgroup(groupname, consumername, ids, count=-1, noack=False, block_ms=-1)`: `ids` is a list of strings (e.g., `['>']`). Returns `StreamReadResult` or `None`.
    *   `xreadgroupSingleId(groupname, consumername, id=">", count=-1, noack=False, block_ms=-1)`: Overload for single ID. Returns `StreamReadResult` or `None`.
    *   `xack(groupname, ids)`: `ids` is a list of message ID strings. Returns count of acknowledged messages.
    *   `xpending(groupname, start="-", end="+", count=-1, consumername="")`: Returns a `RedisReply` object.
    *   `xclaim(groupname, consumername, min_idle_time, ids, justid=False)`: Returns `StreamReadResult` or `None`.
    *   `xlen()`: Returns integer length.
    *   `xtrim(strategy, threshold, approximate=False, limit=0)`: Returns count of trimmed messages.
    *   `xrange(start, end, count=-1)`: Returns `StreamReadResult` or `None`.
    *   `xrevrange(end, start, count=-1)`: Returns `StreamReadResult` or `None`.
    *   `xdel(ids)`: `ids` is a list of message ID strings. Returns count of deleted messages.
    *   `getName()`: Returns the stream name.

**Return Type for Read Operations (`StreamReadResult`)**
The `StreamReadResult` object (wrapping `std::shared_ptr<std::vector<std::pair<std::string, std::vector<std::pair<std::string, std::string>>>>>`) needs to be iterated to get data. It typically behaves like a list of tuples:
`[(stream_name_str, messages_list), ...]`
Each `messages_list` is a list of message tuples:
`[(message_id_str, fields_concatenated_str), ...]`
Example: `("mystream", [("123-0", "f1:v1,f2:v2"), ("123-1", "f3:v3")])`

**`RedisReply` Object (from `xpending`)**
The `RedisReply` object returned by `xpending` has methods to inspect its content:
*   `getTypeStr()`: Returns reply type as string (e.g., "ARRAY", "STRING", "INTEGER").
*   `getElements()`: If type is "ARRAY", returns a list of `RedisReply` objects for sub-elements.
*   `getString()`: If type is "STRING", returns the string value.
*   `getInteger()`: If type is "INTEGER", returns the integer value.
*   And others corresponding to `redisReply` fields.

### Python Code Examples

```python
import unittest # For example structure, normally used in test files
from swsscommon import swsscommon
import time

# Helper to parse "f1:v1,f2:v2" into dict for easier use in Python
def parse_fields_to_dict(fields_str):
    if not fields_str:
        return {}
    return dict(item.split(":", 1) if ":" in item else (item, "") for item in fields_str.split(","))

def main():
    # Ensure SonicDBConfig is loaded if running standalone
    # swsscommon.SonicDBConfig.load_sonic_db_config("/var/run/redis/sonic-db/database_config.json")

    db = swsscommon.DBConnector(swsscommon.SonicDBConfig.getDbId("APPLICATION_DB"), "127.0.0.1", 6379, 0)
    stream_name_py = "mystream_py_example"
    s = swsscommon.Stream(db, stream_name_py)

    print(f"Initial stream length: {s.xlen()}")

    # XADD
    msg_id1 = s.xadd("*", [("sensor", "S1_PY"), ("temp", "28.9")])
    print(f"Added message ID: {msg_id1}")
    msg_id2 = s.xadd("*", [("event", "alert"), ("code", "A001")])
    print(f"Added message ID: {msg_id2}")
    print(f"Current stream length: {s.xlen()}")

    # XRANGE
    print("\nXRANGE results:")
    range_result_ptr = s.xrange("-", "+") # This returns a SWIG proxy for the shared_ptr
    if range_result_ptr:
        # The actual data might be accessed via a method like .get_data() or by iterating the proxy
        # Assuming the SWIG wrapper makes it iterable or provides a method.
        # For this example, let's assume it's directly iterable or has a common conversion.
        # This part depends heavily on the SWIG typemap for StreamReadResultVec.
        # A common pattern is to have a method like `ToList()` or iterate the proxy.
        # If using `%shared_ptr(swss::StreamReadResultVec, StreamReadResult);`
        # SWIG might generate `StreamReadResult` which is the vector itself.
        # Let's assume range_result_ptr is the vector-like object.
        
        # The following is based on typical SWIG vector<pair<...>> wrapping:
        # It will be a list of (stream_name, list_of_messages)
        # list_of_messages is list of (id, fields_str)
        
        # Try to iterate if the object supports it (depends on SWIG)
        # This is a common way SWIG might expose std::vector<std::pair<...>>
        try:
            data = list(range_result_ptr) # If StreamReadResultVec is directly iterable
        except TypeError:
            # Fallback: if it's a custom proxy object, it might have a method
            # For example, if a typemap(out) for shared_ptr<vector<...>> created a Python list directly
            # or if the object has a specific method like ToList() or get_data()
            # This part needs to align with the actual SWIG generated wrapper.
            # Based on the Python test_stream.py, it expects `list(ptr.get_data())`
            # So, if `StreamReadResult` is the type of `range_result_ptr`:
            if hasattr(range_result_ptr, 'get_data'):
                 data = list(range_result_ptr.get_data())
            else: # Fallback if get_data() is not there, try direct iteration
                 data = list(range_result_ptr)


        for stream_name_tuple, messages_tuple_list in data:
            print(f"Stream: {stream_name_tuple}")
            for msg_id, fields_str in messages_tuple_list:
                print(f"  ID: {msg_id}, Fields: {parse_fields_to_dict(fields_str)}")
    else:
        print("  No messages in xrange.")


    # XGROUP CREATE and XREADGROUP
    group_py = "mygroup_py"
    consumer_py = "consumer_py_1"
    
    if s.xgroupCreate(group_py, "0-0", True):
        print(f"\nGroup {group_py} created.")
    else:
        print(f"\nGroup {group_py} creation failed or already exists.")

    print("\nXREADGROUP results:")
    # Read up to 5 new messages
    readgroup_result_ptr = s.xreadgroup(group_py, consumer_py, [">"], 5) 
    ack_ids_py = []
    if readgroup_result_ptr:
        try:
            data_group = list(readgroup_result_ptr.get_data())
        except TypeError:
            data_group = list(readgroup_result_ptr)
            
        for stream_name_tuple, messages_tuple_list in data_group:
            print(f"Stream: {stream_name_tuple}")
            if not messages_tuple_list:
                print(f"  No new messages for consumer {consumer_py}")
            for msg_id, fields_str in messages_tuple_list:
                print(f"  ID: {msg_id} (read by {consumer_py}) Fields: {parse_fields_to_dict(fields_str)}")
                ack_ids_py.append(msg_id)
    else:
        print(f"  No new messages for consumer {consumer_py}")

    # XACK
    if ack_ids_py:
        acked_count = s.xack(group_py, ack_ids_py)
        print(f"\nAcknowledged {acked_count} messages.")

    # XPENDING (summary)
    pending_reply = s.xpending(group_py) # Default args give summary
    if pending_reply:
        print("\nXPENDING Summary:")
        if pending_reply.getTypeStr() == "ARRAY":
            elements = pending_reply.getElements() # List of RedisReply objects
            print(f"  Pending messages: {elements[0].getInteger()}")
            print(f"  Min ID: {elements[1].getString()}")
            print(f"  Max ID: {elements[2].getString()}")
            if len(elements) > 3 and elements[3].getTypeStr() == "ARRAY": # Consumer details
                print("  Consumers with pending messages:")
                for cons_info_reply in elements[3].getElements():
                    cons_details = cons_info_reply.getElements()
                    print(f"    - Name: {cons_details[0].getString()}, Count: {cons_details[1].getString()}")
        else:
            print(f"  Unexpected XPENDING reply type: {pending_reply.getTypeStr()}")

    # Clean up (optional)
    # db.delete(stream_name_py) # DBConnector has 'delete', not 'del' for direct key deletion
    # s.xgroupDestroy(group_py)

if __name__ == "__main__":
    main()
```

## Testing Strategy

Unit tests have been implemented to ensure the correctness of the Redis Streams functionality.

*   **C++ Unit Tests (`tests/redis_stream_ut.cpp`)**:
    *   These tests use the Google Test framework (`gtest`).
    *   A test fixture (`RedisStreamTest`) is used to set up a `DBConnector` to a local Redis instance (APP_DB) and flush the database before each test for isolation.
    *   Tests cover individual stream commands provided by the `swss::Stream` class, which implicitly tests the underlying `DBConnector` methods.
    *   Coverage includes:
        *   `xadd` (auto-ID, specific ID), `xlen`.
        *   `xtrim` (MAXLEN, MINID strategies, approximate trimming, LIMIT).
        *   `xdel` (single, multiple, non-existent IDs).
        *   `xrange`, `xrevrange` (full/partial ranges, COUNT).
        *   `xread` (single stream, COUNT, BLOCK timeout).
        *   `xread` with multiple streams (using `DBConnector` directly).
        *   Consumer group operations: `xgroup_create` (with/without `MKSTREAM`), `xgroup_destroy`, `xgroup_delconsumer`.
        *   `xreadgroup` (reading as consumer, `NOACK` option).
        *   `xack` and verification of pending message removal.
        *   `xpending` (summary and detailed views, with parsing of `RedisReply`).
        *   `xclaim` (claiming messages, `JUSTID` option).
    *   Unique stream names are generated for each test to prevent interference.

*   **Python Unit Tests (`tests/test_stream.py`)**:
    *   These tests use Python's `unittest` framework.
    *   A test class (`TestStream`) sets up a connection to Redis and flushes the database before each test.
    *   Tests mirror the C++ unit tests in scope, verifying the functionality of the SWIG-generated Python `Stream` class.
    *   They check parameter passing and return value conversion between Python and C++, especially for complex data structures like stream messages and `RedisReply` objects from `xpending`.
    *   Helper functions are used in tests to parse concatenated field strings from messages into Python dictionaries for easier assertions.

Both C++ and Python tests require a running Redis instance accessible on `localhost:6379`. They are designed to be run as part of the standard `make check` process.

## Go API

(This section would be added once Go bindings are successfully implemented. It would follow a similar structure to the Python API section, detailing Go data structures, `Stream` methods, and Go code examples.)

---

This documentation provides a comprehensive overview of the Redis Streams support in SWSS. For more detailed information, refer to the source code and specific header files mentioned.The documentation file `doc/redis_streams.md` has been created. It covers the design overview, C++ API (DBConnector extensions and Stream class), C API, Python API, and the testing strategy. Code examples for C++, C, and Python are included for common operations.

The Python examples make a note about the potential need to adjust how `StreamReadResult` (the shared_ptr to vector of pairs) is accessed, depending on the specifics of the SWIG Python wrapper generation (e.g., using `list(ptr.get_data())` as a placeholder).

The Go API section is noted as pending.

Next, I need to update `doc/Makefile.am` to include this new documentation file. I'll check the existing `Makefile.am` for patterns.
