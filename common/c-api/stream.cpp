#include <vector>
#include <string>
#include <memory> // For std::shared_ptr

#include "common/c-api/stream.h"
#include "common/stream.h" // C++ Stream class
#include "common/dbconnector.h" // For swss::DBConnector
#include "common/logger.h"    // For SWSS_LOG_ERROR etc.
#include "common/redisreply.h" // For swss::RedisReply, used by XPENDING

// Helper to convert C-style field-value pairs to C++ vector
static std::vector<std::pair<std::string, std::string>> to_cpp_fvs(const swss_stream_field_value_pair_t *fvs, size_t fv_count) {
    std::vector<std::pair<std::string, std::string>> cpp_fvs;
    if (fvs) {
        for (size_t i = 0; i < fv_count; ++i) {
            cpp_fvs.emplace_back(fvs[i].field ? fvs[i].field : "", fvs[i].value ? fvs[i].value : "");
        }
    }
    return cpp_fvs;
}

// Helper to convert C-style string array to C++ vector<string>
static std::vector<std::string> to_cpp_string_vector(const char *const *arr, size_t count) {
    std::vector<std::string> vec;
    if (arr) {
        for (size_t i = 0; i < count; ++i) {
            vec.emplace_back(arr[i] ? arr[i] : "");
        }
    }
    return vec;
}

// Helper to duplicate string, C-style
static char* c_strdup(const std::string& s) {
    char *dst = new (std::nothrow) char[s.length() + 1];
    if (!dst) return nullptr;
    memcpy(dst, s.c_str(), s.length() + 1);
    return dst;
}


extern "C" {

swss_rc_t swss_stream_create(swss_dbconnector_t *db, const char *stream_name, swss_stream_t **stream) {
    SWSS_LOG_ENTER();
    if (!db || !stream_name || !stream) {
        return SWSS_RC_INVALID_PARAM;
    }
    try {
        swss::DBConnector *cpp_db = reinterpret_cast<swss::DBConnector *>(db);
        swss::Stream *cpp_stream = new swss::Stream(cpp_db, stream_name);
        *stream = reinterpret_cast<swss_stream_t *>(cpp_stream);
    } catch (const std::exception &e) {
        SWSS_LOG_ERROR("Exception in swss_stream_create: %s", e.what());
        return SWSS_RC_GENERAL_ERROR;
    }
    return SWSS_RC_SUCCESS;
}

swss_rc_t swss_stream_free(swss_stream_t *stream) {
    SWSS_LOG_ENTER();
    if (!stream) {
        return SWSS_RC_INVALID_PARAM;
    }
    delete reinterpret_cast<swss::Stream *>(stream);
    return SWSS_RC_SUCCESS;
}

swss_rc_t swss_stream_xadd(swss_stream_t *stream, const char *id_spec,
                           const swss_stream_field_value_pair_t *fvs, size_t fv_count,
                           int maxlen, bool approximate_trim,
                           char **actual_message_id) {
    SWSS_LOG_ENTER();
    if (!stream || !id_spec || (fv_count > 0 && !fvs) || !actual_message_id) {
        return SWSS_RC_INVALID_PARAM;
    }
    *actual_message_id = nullptr;
    try {
        swss::Stream *cpp_stream = reinterpret_cast<swss::Stream *>(stream);
        std::vector<std::pair<std::string, std::string>> cpp_fvs = to_cpp_fvs(fvs, fv_count);
        std::string cpp_id = cpp_stream->xadd(id_spec, cpp_fvs, maxlen, approximate_trim);
        if (!cpp_id.empty()) {
            *actual_message_id = c_strdup(cpp_id);
            if (!*actual_message_id) return SWSS_RC_NO_MEMORY;
        }
    } catch (const std::exception &e) {
        SWSS_LOG_ERROR("Exception in swss_stream_xadd: %s", e.what());
        return SWSS_RC_GENERAL_ERROR;
    }
    return SWSS_RC_SUCCESS;
}

// Common helper for processing stream message results from C++ to C
static swss_rc_t process_cpp_stream_results(
    std::shared_ptr<std::vector<std::pair<std::string, std::vector<std::pair<std::string, std::string>>>>> cpp_results_ptr,
    swss_stream_messages_t **c_results,
    size_t *c_result_count)
{
    if (!cpp_results_ptr || cpp_results_ptr->empty()) {
        *c_results = nullptr;
        *c_result_count = 0;
        return SWSS_RC_SUCCESS;
    }

    auto& cpp_results_vec = *cpp_results_ptr;
    *c_result_count = cpp_results_vec.size();
    *c_results = new (std::nothrow) swss_stream_messages_t[*c_result_count];
    if (!*c_results) {
        *c_result_count = 0;
        return SWSS_RC_NO_MEMORY;
    }
    std::fill(*c_results, *c_results + *c_result_count, swss_stream_messages_t{}); // Zero initialize

    for (size_t i = 0; i < *c_result_count; ++i) {
        (*c_results)[i].stream_name = c_strdup(cpp_results_vec[i].first);
        if (!(*c_results)[i].stream_name) goto cleanup_error;

        auto& cpp_messages_vec = cpp_results_vec[i].second;
        (*c_results)[i].message_count = cpp_messages_vec.size();
        if (cpp_messages_vec.empty()) {
            (*c_results)[i].messages = nullptr;
            continue;
        }

        (*c_results)[i].messages = new (std::nothrow) swss_stream_message_entry_t[cpp_messages_vec.size()];
        if (!(*c_results)[i].messages) {
            (*c_results)[i].message_count = 0; // Correct the count before cleanup
            goto cleanup_error;
        }
        std::fill((*c_results)[i].messages, (*c_results)[i].messages + cpp_messages_vec.size(), swss_stream_message_entry_t{});

        for (size_t j = 0; j < cpp_messages_vec.size(); ++j) {
            (*c_results)[i].messages[j].message_id = c_strdup(cpp_messages_vec[j].first);
            (*c_results)[i].messages[j].fields_concatenated = c_strdup(cpp_messages_vec[j].second);
            if (!(*c_results)[i].messages[j].message_id || !(*c_results)[i].messages[j].fields_concatenated) {
                 goto cleanup_error;
            }
        }
    }
    return SWSS_RC_SUCCESS;

cleanup_error:
    swss_stream_free_messages_results(*c_results, *c_result_count);
    *c_results = nullptr;
    *c_result_count = 0;
    return SWSS_RC_NO_MEMORY;
}


swss_rc_t swss_stream_xread(swss_stream_t *stream,
                            const char *const *ids, size_t id_count,
                            int count_per_stream, int block_ms,
                            swss_stream_messages_t **results, size_t *result_count) {
    SWSS_LOG_ENTER();
    if (!stream || !ids || id_count == 0 || !results || !result_count) {
        return SWSS_RC_INVALID_PARAM;
    }
    *results = nullptr;
    *result_count = 0;
    if (id_count != 1) { // This simplified C API assumes xread on the single stream object.
        SWSS_LOG_ERROR("swss_stream_xread: id_count must be 1 for this API version.");
        return SWSS_RC_INVALID_PARAM;
    }
    try {
        swss::Stream *cpp_stream = reinterpret_cast<swss::Stream *>(stream);
        std::vector<std::string> cpp_ids = to_cpp_string_vector(ids, id_count);
        
        auto cpp_results_ptr = cpp_stream->xread(cpp_ids, count_per_stream, block_ms);
        return process_cpp_stream_results(cpp_results_ptr, results, result_count);

    } catch (const std::exception &e) {
        SWSS_LOG_ERROR("Exception in swss_stream_xread: %s", e.what());
        return SWSS_RC_GENERAL_ERROR;
    }
}


swss_rc_t swss_stream_xgroup_create(swss_stream_t *stream, const char *group_name, const char *id_spec, bool mkstream, int *result_code) {
    SWSS_LOG_ENTER();
    if (!stream || !group_name || !id_spec || !result_code) {
        return SWSS_RC_INVALID_PARAM;
    }
    try {
        swss::Stream *cpp_stream = reinterpret_cast<swss::Stream *>(stream);
        bool success = cpp_stream->xgroup_create(group_name, id_spec, mkstream);
        *result_code = success ? 0 : -1; // 0 for OK, Redis typically returns string "OK" or error.
                                         // The C++ layer converts this to bool.
    } catch (const std::exception &e) {
        SWSS_LOG_ERROR("Exception in swss_stream_xgroup_create: %s", e.what());
        *result_code = -1; // Indicate error
        return SWSS_RC_GENERAL_ERROR;
    }
    return SWSS_RC_SUCCESS;
}

swss_rc_t swss_stream_xgroup_destroy(swss_stream_t *stream, const char *group_name, int *result_code) {
    SWSS_LOG_ENTER();
    if (!stream || !group_name || !result_code) {
        return SWSS_RC_INVALID_PARAM;
    }
    try {
        swss::Stream *cpp_stream = reinterpret_cast<swss::Stream *>(stream);
        bool success = cpp_stream->xgroup_destroy(group_name);
        *result_code = success ? 1 : 0; // As per C++ layer (1 if destroyed, 0 if not found)
    } catch (const std::exception &e) {
        SWSS_LOG_ERROR("Exception in swss_stream_xgroup_destroy: %s", e.what());
        *result_code = -1; // Indicate error
        return SWSS_RC_GENERAL_ERROR;
    }
    return SWSS_RC_SUCCESS;
}

swss_rc_t swss_stream_xgroup_delconsumer(swss_stream_t *stream, const char *group_name, const char *consumer_name, int64_t *deleted_count) {
    SWSS_LOG_ENTER();
    if (!stream || !group_name || !consumer_name || !deleted_count) {
        return SWSS_RC_INVALID_PARAM;
    }
    try {
        swss::Stream *cpp_stream = reinterpret_cast<swss::Stream *>(stream);
        *deleted_count = cpp_stream->xgroup_delconsumer(group_name, consumer_name);
    } catch (const std::exception &e) {
        SWSS_LOG_ERROR("Exception in swss_stream_xgroup_delconsumer: %s", e.what());
        return SWSS_RC_GENERAL_ERROR;
    }
    return SWSS_RC_SUCCESS;
}

swss_rc_t swss_stream_xreadgroup(swss_stream_t *stream,
                                 const char *group_name, const char *consumer_name,
                                 const char *const *ids, size_t id_count,
                                 int count_per_stream, bool noack, int block_ms,
                                 swss_stream_messages_t **results, size_t *result_count) {
    SWSS_LOG_ENTER();
    if (!stream || !group_name || !consumer_name || !ids || id_count == 0 || !results || !result_count) {
        return SWSS_RC_INVALID_PARAM;
    }
     *results = nullptr;
    *result_count = 0;
    if (id_count != 1) { // This simplified C API assumes xreadgroup on the single stream object.
        SWSS_LOG_ERROR("swss_stream_xreadgroup: id_count must be 1 for this API version.");
        return SWSS_RC_INVALID_PARAM;
    }
    try {
        swss::Stream *cpp_stream = reinterpret_cast<swss::Stream *>(stream);
        std::vector<std::string> cpp_ids = to_cpp_string_vector(ids, id_count);

        auto cpp_results_ptr = cpp_stream->xreadgroup(group_name, consumer_name, cpp_ids, count_per_stream, noack, block_ms);
        return process_cpp_stream_results(cpp_results_ptr, results, result_count);

    } catch (const std::exception &e) {
        SWSS_LOG_ERROR("Exception in swss_stream_xreadgroup: %s", e.what());
        return SWSS_RC_GENERAL_ERROR;
    }
}

swss_rc_t swss_stream_xack(swss_stream_t *stream, const char *group_name, const char *const *message_ids, size_t id_count, int64_t *acked_count) {
    SWSS_LOG_ENTER();
    if (!stream || !group_name || (id_count > 0 && !message_ids) || !acked_count) {
        return SWSS_RC_INVALID_PARAM;
    }
    try {
        swss::Stream *cpp_stream = reinterpret_cast<swss::Stream *>(stream);
        std::vector<std::string> cpp_ids = to_cpp_string_vector(message_ids, id_count);
        *acked_count = cpp_stream->xack(group_name, cpp_ids);
    } catch (const std::exception &e) {
        SWSS_LOG_ERROR("Exception in swss_stream_xack: %s", e.what());
        return SWSS_RC_GENERAL_ERROR;
    }
    return SWSS_RC_SUCCESS;
}


// XPENDING is complex. The C++ layer returns a shared_ptr<RedisReply>.
// We need to parse this reply.
// For summary: [pending_count (int), min_id (str), max_id (str), [consumer_info_array (may be nil or array)]]
// For detailed: [[id, consumer, idle, deliveries], ...]
swss_rc_t swss_stream_xpending_summary(swss_stream_t *stream, const char *group_name,
                                       swss_stream_pending_summary_t *summary) {
    SWSS_LOG_ENTER();
    if (!stream || !group_name || !summary) {
        return SWSS_RC_INVALID_PARAM;
    }
    *summary = {}; // Zero initialize
    try {
        swss::Stream *cpp_stream = reinterpret_cast<swss::Stream *>(stream);
        // The C++ API returns a shared_ptr<RedisReply>
        // The detailed parsing is not done in the C++ Stream class, but in DBConnector's xpending.
        // DBConnector::xpending returns a shared_ptr<RedisReply> which is a raw redisReply*.
        // This means we need to parse the redisReply* here.
        std::shared_ptr<swss::RedisReply> reply_ptr = cpp_stream->xpending(group_name);
        if (!reply_ptr) {
            SWSS_LOG_ERROR("swss_stream_xpending_summary: got null RedisReply pointer.");
            return SWSS_RC_UNEXPECTED_RESPONSE;
        }
        redisReply *r = reply_ptr->getContext(); // Get the raw hiredis reply context

        if (!r || r->type != REDIS_REPLY_ARRAY || r->elements < 3) { // Expecting at least 3 for summary
            SWSS_LOG_ERROR("swss_stream_xpending_summary: Invalid reply type or structure. Expected array with >= 3 elements. Type: %d, Elements: %zu", r ? r->type : -1, r ? r->elements : 0);
            return SWSS_RC_UNEXPECTED_RESPONSE;
        }

        if (r->element[0]->type == REDIS_REPLY_INTEGER) {
            summary->pending_message_count = r->element[0]->integer;
        } else { goto parse_error; }

        if (r->element[1]->type == REDIS_REPLY_STRING) {
            summary->min_message_id = c_strdup(r->element[1]->str);
        } else if (r->element[1]->type == REDIS_REPLY_NIL) {
            summary->min_message_id = nullptr; // No min ID if count is 0
        } else { goto parse_error; }

        if (r->element[2]->type == REDIS_REPLY_STRING) {
            summary->max_message_id = c_strdup(r->element[2]->str);
        } else if (r->element[2]->type == REDIS_REPLY_NIL) {
            summary->max_message_id = nullptr; // No max ID if count is 0
        } else { goto parse_error; }
        
        // Element 3 (consumer details) is more complex and omitted for now for simplicity as per header file note.
        // if (r->elements > 3 && r->element[3]->type == REDIS_REPLY_ARRAY) { ... }

        if ((summary->min_message_id && !summary->min_message_id && r->element[1]->type == REDIS_REPLY_STRING) ||
            (summary->max_message_id && !summary->max_message_id && r->element[2]->type == REDIS_REPLY_STRING) ) {
            swss_stream_free_pending_summary(summary); // partial alloc cleanup
            return SWSS_RC_NO_MEMORY;
        }

    } catch (const std::exception &e) {
        SWSS_LOG_ERROR("Exception in swss_stream_xpending_summary: %s", e.what());
        swss_stream_free_pending_summary(summary); // cleanup
        return SWSS_RC_GENERAL_ERROR;
    }
    return SWSS_RC_SUCCESS;

parse_error:
    SWSS_LOG_ERROR("swss_stream_xpending_summary: Reply parsing error.");
    swss_stream_free_pending_summary(summary); // cleanup
    return SWSS_RC_UNEXPECTED_RESPONSE;
}


swss_rc_t swss_stream_xpending_detailed(swss_stream_t *stream, const char *group_name,
                                        const char *start_id, const char *end_id, int count,
                                        const char *consumer_name,
                                        swss_stream_pending_entry_t **entries, size_t *entry_count) {
    SWSS_LOG_ENTER();
     if (!stream || !group_name || !start_id || !end_id || !entries || !entry_count) {
        return SWSS_RC_INVALID_PARAM;
    }
    *entries = nullptr;
    *entry_count = 0;

    try {
        swss::Stream *cpp_stream = reinterpret_cast<swss::Stream *>(stream);
        std::shared_ptr<swss::RedisReply> reply_ptr = cpp_stream->xpending(group_name, start_id, end_id, count, consumer_name ? consumer_name : "");
        
        if (!reply_ptr) {
            SWSS_LOG_ERROR("swss_stream_xpending_detailed: got null RedisReply pointer.");
            return SWSS_RC_UNEXPECTED_RESPONSE;
        }
        redisReply *r = reply_ptr->getContext();

        if (!r || r->type != REDIS_REPLY_ARRAY) {
             SWSS_LOG_ERROR("swss_stream_xpending_detailed: Invalid reply type. Expected array. Type: %d", r ? r->type : -1);
            return SWSS_RC_UNEXPECTED_RESPONSE;
        }

        *entry_count = r->elements;
        if (*entry_count == 0) {
            *entries = nullptr;
            return SWSS_RC_SUCCESS;
        }

        *entries = new (std::nothrow) swss_stream_pending_entry_t[*entry_count];
        if (!*entries) {
            *entry_count = 0;
            return SWSS_RC_NO_MEMORY;
        }
        std::fill(*entries, *entries + *entry_count, swss_stream_pending_entry_t{}); // Zero initialize

        for (size_t i = 0; i < *entry_count; ++i) {
            redisReply *item = r->element[i];
            if (item->type != REDIS_REPLY_ARRAY || item->elements != 4) goto parse_error_detailed;

            if (item->element[0]->type == REDIS_REPLY_STRING) {
                 (*entries)[i].message_id = c_strdup(item->element[0]->str);
            } else { goto parse_error_detailed; }
            
            if (item->element[1]->type == REDIS_REPLY_STRING) {
                (*entries)[i].consumer_name = c_strdup(item->element[1]->str);
            } else { goto parse_error_detailed; }

            if (item->element[2]->type == REDIS_REPLY_INTEGER) {
                (*entries)[i].idle_time_ms = item->element[2]->integer;
            } else { goto parse_error_detailed; }

            if (item->element[3]->type == REDIS_REPLY_INTEGER) {
                (*entries)[i].delivery_count = item->element[3]->integer;
            } else { goto parse_error_detailed; }

            if (!(*entries)[i].message_id || !(*entries)[i].consumer_name) {
                swss_stream_free_pending_entries(*entries, *entry_count);
                *entries = nullptr;
                *entry_count = 0;
                return SWSS_RC_NO_MEMORY;
            }
        }

    } catch (const std::exception &e) {
        SWSS_LOG_ERROR("Exception in swss_stream_xpending_detailed: %s", e.what());
        swss_stream_free_pending_entries(*entries, *entry_count);
        *entries = nullptr;
        *entry_count = 0;
        return SWSS_RC_GENERAL_ERROR;
    }
    return SWSS_RC_SUCCESS;

parse_error_detailed:
    SWSS_LOG_ERROR("swss_stream_xpending_detailed: Reply parsing error for an entry.");
    swss_stream_free_pending_entries(*entries, *entry_count);
    *entries = nullptr;
    *entry_count = 0;
    return SWSS_RC_UNEXPECTED_RESPONSE;
}


swss_rc_t swss_stream_xclaim(swss_stream_t *stream, const char *group_name, const char *consumer_name,
                             int min_idle_time_ms, const char *const *message_ids, size_t id_count, bool justid,
                             swss_stream_messages_t **results, size_t *result_count) {
    SWSS_LOG_ENTER();
    if (!stream || !group_name || !consumer_name || (id_count > 0 && !message_ids) || !results || !result_count) {
        return SWSS_RC_INVALID_PARAM;
    }
    *results = nullptr;
    *result_count = 0;
    try {
        swss::Stream *cpp_stream = reinterpret_cast<swss::Stream *>(stream);
        std::vector<std::string> cpp_ids = to_cpp_string_vector(message_ids, id_count);
        
        auto cpp_results_ptr = cpp_stream->xclaim(group_name, consumer_name, min_idle_time_ms, cpp_ids, justid);
        // The C++ xclaim returns the same structure as xread.
        return process_cpp_stream_results(cpp_results_ptr, results, result_count);

    } catch (const std::exception &e) {
        SWSS_LOG_ERROR("Exception in swss_stream_xclaim: %s", e.what());
        return SWSS_RC_GENERAL_ERROR;
    }
}

swss_rc_t swss_stream_xlen(swss_stream_t *stream, int64_t *length) {
    SWSS_LOG_ENTER();
    if (!stream || !length) {
        return SWSS_RC_INVALID_PARAM;
    }
    try {
        swss::Stream *cpp_stream = reinterpret_cast<swss::Stream *>(stream);
        *length = cpp_stream->xlen();
    } catch (const std::exception &e) {
        SWSS_LOG_ERROR("Exception in swss_stream_xlen: %s", e.what());
        return SWSS_RC_GENERAL_ERROR;
    }
    return SWSS_RC_SUCCESS;
}

swss_rc_t swss_stream_xtrim(swss_stream_t *stream, const char *strategy,
                            const char *threshold, bool approximate_trim, int limit,
                            int64_t *trimmed_count) {
    SWSS_LOG_ENTER();
    if (!stream || !strategy || !threshold || !trimmed_count) {
        return SWSS_RC_INVALID_PARAM;
    }
    try {
        swss::Stream *cpp_stream = reinterpret_cast<swss::Stream *>(stream);
        *trimmed_count = cpp_stream->xtrim(strategy, threshold, approximate_trim, limit);
    } catch (const std::exception &e) {
        SWSS_LOG_ERROR("Exception in swss_stream_xtrim: %s", e.what());
        return SWSS_RC_GENERAL_ERROR;
    }
    return SWSS_RC_SUCCESS;
}


swss_rc_t swss_stream_xrange(swss_stream_t *stream, const char *start_id, const char *end_id, int count,
                             swss_stream_messages_t **results, size_t *result_count) {
    SWSS_LOG_ENTER();
    if (!stream || !start_id || !end_id || !results || !result_count) {
        return SWSS_RC_INVALID_PARAM;
    }
    *results = nullptr;
    *result_count = 0;
    try {
        swss::Stream *cpp_stream = reinterpret_cast<swss::Stream *>(stream);
        auto cpp_results_ptr = cpp_stream->xrange(start_id, end_id, count);
        return process_cpp_stream_results(cpp_results_ptr, results, result_count);
    } catch (const std::exception &e) {
        SWSS_LOG_ERROR("Exception in swss_stream_xrange: %s", e.what());
        return SWSS_RC_GENERAL_ERROR;
    }
}

swss_rc_t swss_stream_xrevrange(swss_stream_t *stream, const char *end_id, const char *start_id, int count,
                                swss_stream_messages_t **results, size_t *result_count) {
    SWSS_LOG_ENTER();
    if (!stream || !start_id || !end_id || !results || !result_count) {
        return SWSS_RC_INVALID_PARAM;
    }
    *results = nullptr;
    *result_count = 0;
    try {
        swss::Stream *cpp_stream = reinterpret_cast<swss::Stream *>(stream);
        auto cpp_results_ptr = cpp_stream->xrevrange(end_id, start_id, count);
        return process_cpp_stream_results(cpp_results_ptr, results, result_count);
    } catch (const std::exception &e) {
        SWSS_LOG_ERROR("Exception in swss_stream_xrevrange: %s", e.what());
        return SWSS_RC_GENERAL_ERROR;
    }
}

swss_rc_t swss_stream_xdel(swss_stream_t *stream, const char *const *message_ids, size_t id_count, int64_t *deleted_count) {
    SWSS_LOG_ENTER();
    if (!stream || (id_count > 0 && !message_ids) || !deleted_count) {
        return SWSS_RC_INVALID_PARAM;
    }
    try {
        swss::Stream *cpp_stream = reinterpret_cast<swss::Stream *>(stream);
        std::vector<std::string> cpp_ids = to_cpp_string_vector(message_ids, id_count);
        *deleted_count = cpp_stream->xdel(cpp_ids);
    } catch (const std::exception &e) {
        SWSS_LOG_ERROR("Exception in swss_stream_xdel: %s", e.what());
        return SWSS_RC_GENERAL_ERROR;
    }
    return SWSS_RC_SUCCESS;
}


// Freeing functions
swss_rc_t swss_stream_free_string(char *str) {
    if (str) delete[] str;
    return SWSS_RC_SUCCESS;
}

swss_rc_t swss_stream_free_message_entries(swss_stream_message_entry_t *entries, size_t count) {
    if (!entries) return SWSS_RC_SUCCESS;
    for (size_t i = 0; i < count; ++i) {
        swss_stream_free_string(entries[i].message_id);
        swss_stream_free_string(entries[i].fields_concatenated);
    }
    delete[] entries;
    return SWSS_RC_SUCCESS;
}

swss_rc_t swss_stream_free_messages_results(swss_stream_messages_t *results, size_t count) {
    if (!results) return SWSS_RC_SUCCESS;
    for (size_t i = 0; i < count; ++i) {
        swss_stream_free_string(results[i].stream_name);
        swss_stream_free_message_entries(results[i].messages, results[i].message_count);
    }
    delete[] results;
    return SWSS_RC_SUCCESS;
}

swss_rc_t swss_stream_free_pending_summary(swss_stream_pending_summary_t *summary) {
    if (!summary) return SWSS_RC_SUCCESS;
    swss_stream_free_string(summary->min_message_id);
    swss_stream_free_string(summary->max_message_id);
    // Free consumer_stats if it were implemented
    return SWSS_RC_SUCCESS;
}

swss_rc_t swss_stream_free_pending_entries(swss_stream_pending_entry_t *entries, size_t count) {
    if (!entries) return SWSS_RC_SUCCESS;
    for (size_t i = 0; i < count; ++i) {
        swss_stream_free_string(entries[i].message_id);
        swss_stream_free_string(entries[i].consumer_name);
    }
    delete[] entries;
    return SWSS_RC_SUCCESS;
}

} // extern "C"
