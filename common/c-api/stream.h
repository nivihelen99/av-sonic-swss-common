#ifndef SWSS_C_API_STREAM_H
#define SWSS_C_API_STREAM_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#include "dbconnector.h" // For swss_dbconnector_t
#include "result.h"      // For swss_rc_t and common result codes

#ifdef __cplusplus
extern "C" {
#endif

typedef struct swss_stream_s swss_stream_t;

// For XADD field-value pairs
// Similar to swss_field_value_pair_t in table.h but repeated here for stream context
// if there are subtle differences or to keep modules distinct.
// For now, assuming it's the same structure.
typedef struct {
    const char *field;
    const char *value;
} swss_stream_field_value_pair_t;

// Structure to represent a single message from a stream
typedef struct {
    char *message_id;   // Message ID
    char *fields_concatenated; // Fields and values concatenated (e.g., "field1:value1,field2:value2")
    // Alternatively, could be swss_stream_field_value_pair_t *fvs; size_t fv_count;
    // But sticking to concatenated string as per C++ DBConnector implementation.
} swss_stream_message_entry_t;

// Structure to represent messages read from a stream (or multiple streams in raw XREAD)
typedef struct {
    char *stream_name; // The name of the stream these messages belong to
    swss_stream_message_entry_t *messages;
    size_t message_count;
} swss_stream_messages_t;


// Structures for XPENDING reply
// Summary (when no specific range/consumer is given)
typedef struct {
    int64_t pending_message_count;
    char *min_message_id;
    char *max_message_id;
    // List of consumers with pending messages and their counts
    // This is a complex part; for simplicity, we might initially omit detailed consumer list from summary
    // or provide a function to get consumer-specific pending details.
    // For now, focusing on the main counts and IDs.
    // To fully represent, it would be:
    // struct { char* consumer_name; int64_t pending_count; } *consumer_stats;
    // size_t consumer_stats_count;
} swss_stream_pending_summary_t;

// Detailed entry (when a range and optional consumer are specified)
typedef struct {
    char *message_id;
    char *consumer_name;
    int64_t idle_time_ms;
    int64_t delivery_count;
} swss_stream_pending_entry_t;


// Function to create a Stream object
swss_rc_t swss_stream_create(swss_dbconnector_t *db, const char *stream_name, swss_stream_t **stream);

// Function to free a Stream object
swss_rc_t swss_stream_free(swss_stream_t *stream);

// Stream commands
swss_rc_t swss_stream_xadd(swss_stream_t *stream, const char *id_spec, /* e.g., "*" or specific ID */
                           const swss_stream_field_value_pair_t *fvs, size_t fv_count,
                           int maxlen, bool approximate_trim,
                           char **actual_message_id); // Output: actual message ID, callee allocates, caller frees with swss_stream_free_string

swss_rc_t swss_stream_xread(swss_stream_t *stream,
                            const char *const *ids, /* Array of IDs, e.g. {"0-0", "$"} corresponding to stream names */
                            size_t id_count, /* Must match stream_count from swss_stream_create or an explicit list */
                            int count_per_stream, int block_ms,
                            swss_stream_messages_t **results, /* Output: Array of results, one per stream with messages */
                            size_t *result_count); /* Output: Number of streams in results that had messages */
                            // Note: For this simplified C API tied to a single stream, id_count will typically be 1.
                            // For multi-stream xread, a different C API structure might be needed, or keys passed explicitly.
                            // This xread is for the stream object's m_streamName.

swss_rc_t swss_stream_xgroup_create(swss_stream_t *stream, const char *group_name, const char *id_spec, bool mkstream, int *result_code); // 0 for OK, specific error for others
swss_rc_t swss_stream_xgroup_destroy(swss_stream_t *stream, const char *group_name, int *result_code); // 1 for success, 0 if group not found
swss_rc_t swss_stream_xgroup_delconsumer(swss_stream_t *stream, const char *group_name, const char *consumer_name, int64_t *deleted_count);

swss_rc_t swss_stream_xreadgroup(swss_stream_t *stream,
                                 const char *group_name, const char *consumer_name,
                                 const char *const *ids, /* Array of IDs, e.g. {">"} for this stream */
                                 size_t id_count, /* Typically 1 for this API */
                                 int count_per_stream, bool noack, int block_ms,
                                 swss_stream_messages_t **results, /* Output: Array of results, one per stream with messages */
                                 size_t *result_count); /* Output: Number of streams in results (typically 1) */

swss_rc_t swss_stream_xack(swss_stream_t *stream, const char *group_name, const char *const *message_ids, size_t id_count, int64_t *acked_count);

// For XPENDING, we might need two functions: one for summary, one for detailed list.
swss_rc_t swss_stream_xpending_summary(swss_stream_t *stream, const char *group_name,
                                       swss_stream_pending_summary_t *summary); // Output

swss_rc_t swss_stream_xpending_detailed(swss_stream_t *stream, const char *group_name,
                                        const char *start_id, const char *end_id, int count,
                                        const char *consumer_name, /* Optional */
                                        swss_stream_pending_entry_t **entries, /* Output: Array of pending entries */
                                        size_t *entry_count); /* Output: Number of entries */

swss_rc_t swss_stream_xclaim(swss_stream_t *stream, const char *group_name, const char *consumer_name,
                             int min_idle_time_ms, const char *const *message_ids, size_t id_count, bool justid,
                             swss_stream_messages_t **results, /* Output: Array of claimed messages */
                             size_t *result_count); /* Output: Number of streams in results (typically 1, holding claimed messages) */

swss_rc_t swss_stream_xlen(swss_stream_t *stream, int64_t *length);

swss_rc_t swss_stream_xtrim(swss_stream_t *stream, const char *strategy, /* "MAXLEN" or "MINID" */
                            const char *threshold, bool approximate_trim, int limit,
                            int64_t *trimmed_count);

swss_rc_t swss_stream_xrange(swss_stream_t *stream, const char *start_id, const char *end_id, int count,
                             swss_stream_messages_t **results, /* Output: Array of messages */
                             size_t *result_count); /* Output: Number of streams in results (typically 1) */

swss_rc_t swss_stream_xrevrange(swss_stream_t *stream, const char *end_id, const char *start_id, int count,
                                swss_stream_messages_t **results, /* Output: Array of messages */
                                size_t *result_count); /* Output: Number of streams in results (typically 1) */

swss_rc_t swss_stream_xdel(swss_stream_t *stream, const char *const *message_ids, size_t id_count, int64_t *deleted_count);


// Utility functions for freeing memory allocated by the API
swss_rc_t swss_stream_free_string(char *str);
swss_rc_t swss_stream_free_message_entries(swss_stream_message_entry_t *entries, size_t count);
swss_rc_t swss_stream_free_messages_results(swss_stream_messages_t *results, size_t count);
swss_rc_t swss_stream_free_pending_summary(swss_stream_pending_summary_t *summary);
swss_rc_t swss_stream_free_pending_entries(swss_stream_pending_entry_t *entries, size_t count);


#ifdef __cplusplus
} // extern "C"
#endif

#endif // SWSS_C_API_STREAM_H
