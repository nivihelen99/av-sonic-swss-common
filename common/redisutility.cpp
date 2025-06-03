#include "redisutility.h"
#include "stringutility.h"
#include "dbconnector.h"
#include "redisreply.h"
#include "rediscommand.h"
#include "logger.h"

#include <vector>
#include <string>
#include <map>
#include <boost/algorithm/string.hpp>

// Note: boost::optional is included via redisutility.h

namespace swss {

boost::optional<std::string> fvsGetValue(
    const std::vector<FieldValueTuple> &fvt,
    const std::string &field,
    bool case_insensitive)
{
    boost::optional<std::string> ret;

    for (auto itr = fvt.begin(); itr != fvt.end(); itr++)
    {
        bool is_equal = false;
        if (case_insensitive)
        {
            is_equal = boost::iequals(fvField(*itr), field);
        }
        else
        {
            is_equal = (fvField(*itr) == field);
        }
        if (is_equal)
        {
            ret = fvValue(*itr);
            break;
        }
    }

    return ret;
}

// Generic Commands
boost::optional<std::string> genericGet(DBConnector* db, const std::string& key) {
    RedisCommand command;
    command.format("GET %s", key.c_str());
    RedisReply r(db->getRedisContext(db->getDbId()), command, REDIS_REPLY_STRING, true);

    if (r.getContext()->err) {
        SWSS_LOG_ERROR("GET command failed for key %s: %s", key.c_str(), r.getContext()->errstr);
        return boost::none;
    }
    if (r.getContext()->type == REDIS_REPLY_NIL) {
        return boost::none;
    }
    return r.get_str();
}

void genericSet(DBConnector* db, const std::string& key, const std::string& value) {
    RedisCommand command;
    command.format("SET %s %s", key.c_str(), value.c_str());
    RedisReply r(db->getRedisContext(db->getDbId()), command);
    if (r.getContext()->err) {
        SWSS_LOG_ERROR("SET command failed for key %s value %s: %s", key.c_str(), value.c_str(), r.getContext()->errstr);
    }
}

bool exists(DBConnector* db, const std::string& key) {
    RedisCommand command;
    command.format("EXISTS %s", key.c_str());
    RedisReply r(db->getRedisContext(db->getDbId()), command, REDIS_REPLY_INTEGER);

    if (r.getContext()->err) {
        SWSS_LOG_ERROR("EXISTS command failed for key %s: %s", key.c_str(), r.getContext()->errstr);
        return false;
    }
    return r.get_longlong() > 0;
}

long long del(DBConnector* db, const std::string& key) {
    RedisCommand command;
    command.format("DEL %s", key.c_str());
    RedisReply r(db->getRedisContext(db->getDbId()), command, REDIS_REPLY_INTEGER);

    if (r.getContext()->err) {
        SWSS_LOG_ERROR("DEL command failed for key %s: %s", key.c_str(), r.getContext()->errstr);
        return 0;
    }
    return r.get_longlong();
}

// Hash Commands
boost::optional<std::string> hget(DBConnector* db, const std::string& hash, const std::string& field) {
    RedisCommand command;
    command.format("HGET %s %s", hash.c_str(), field.c_str());
    RedisReply r(db->getRedisContext(db->getDbId()), command, REDIS_REPLY_STRING, true);

    if (r.getContext()->err) {
        SWSS_LOG_ERROR("HGET command failed for hash %s field %s: %s", hash.c_str(), field.c_str(), r.getContext()->errstr);
        return boost::none;
    }
    if (r.getContext()->type == REDIS_REPLY_NIL) {
        return boost::none;
    }
    return r.get_str();
}

void hset(DBConnector* db, const std::string& hash, const std::string& field, const std::string& value) {
    RedisCommand command;
    command.format("HSET %s %s %s", hash.c_str(), field.c_str(), value.c_str());
    RedisReply r(db->getRedisContext(db->getDbId()), command, REDIS_REPLY_INTEGER);
    if (r.getContext()->err) {
        SWSS_LOG_ERROR("HSET command failed for hash %s field %s value %s: %s", hash.c_str(), field.c_str(), value.c_str(), r.getContext()->errstr);
    }
}

long long hdel(DBConnector* db, const std::string& hash, const std::string& field) {
    RedisCommand command;
    command.format("HDEL %s %s", hash.c_str(), field.c_str());
    RedisReply r(db->getRedisContext(db->getDbId()), command, REDIS_REPLY_INTEGER);

    if (r.getContext()->err) {
        SWSS_LOG_ERROR("HDEL command failed for hash %s field %s: %s", hash.c_str(), field.c_str(), r.getContext()->errstr);
        return 0;
    }
    return r.get_longlong();
}

std::map<std::string, std::string> hgetall(DBConnector* db, const std::string& hash) {
    std::map<std::string, std::string> result;
    RedisCommand command;
    command.format("HGETALL %s", hash.c_str());
    RedisReply r(db->getRedisContext(db->getDbId()), command, REDIS_REPLY_ARRAY);

    if (r.getContext()->err) {
        SWSS_LOG_ERROR("HGETALL command failed for hash %s: %s", hash.c_str(), r.getContext()->errstr);
        return result;
    }

    auto reply = r.getContext();
    for (size_t i = 0; (i + 1) < reply->elements; i += 2) {
        if (reply->element[i]->type == REDIS_REPLY_STRING && reply->element[i+1]->type == REDIS_REPLY_STRING) {
            result[std::string(reply->element[i]->str, reply->element[i]->len)] =
                  std::string(reply->element[i+1]->str, reply->element[i+1]->len);
        }
    }
    return result;
}

// List Commands
long long lpush(DBConnector* db, const std::string& key, const std::string& value) {
    RedisCommand command;
    command.format("LPUSH %s %s", key.c_str(), value.c_str());
    RedisReply r(db->getRedisContext(db->getDbId()), command, REDIS_REPLY_INTEGER);
    if (r.getContext()->err) {
        SWSS_LOG_ERROR("LPUSH command failed for key %s value %s: %s", key.c_str(), value.c_str(), r.getContext()->errstr);
        return 0;
    }
    return r.get_longlong();
}

long long rpush(DBConnector* db, const std::string& key, const std::string& value) {
    RedisCommand command;
    command.format("RPUSH %s %s", key.c_str(), value.c_str());
    RedisReply r(db->getRedisContext(db->getDbId()), command, REDIS_REPLY_INTEGER);
    if (r.getContext()->err) {
        SWSS_LOG_ERROR("RPUSH command failed for key %s value %s: %s", key.c_str(), value.c_str(), r.getContext()->errstr);
        return 0;
    }
    return r.get_longlong();
}

boost::optional<std::string> lpop(DBConnector* db, const std::string& key) {
    RedisCommand command;
    command.format("LPOP %s", key.c_str());
    RedisReply r(db->getRedisContext(db->getDbId()), command, REDIS_REPLY_STRING, true);

    if (r.getContext()->err) {
        SWSS_LOG_ERROR("LPOP command failed for key %s: %s", key.c_str(), r.getContext()->errstr);
        return boost::none;
    }
    if (r.getContext()->type == REDIS_REPLY_NIL) {
        return boost::none;
    }
    return r.get_str();
}

boost::optional<std::string> rpop(DBConnector* db, const std::string& key) {
    RedisCommand command;
    command.format("RPOP %s", key.c_str());
    RedisReply r(db->getRedisContext(db->getDbId()), command, REDIS_REPLY_STRING, true);

    if (r.getContext()->err) {
        SWSS_LOG_ERROR("RPOP command failed for key %s: %s", key.c_str(), r.getContext()->errstr);
        return boost::none;
    }
    if (r.getContext()->type == REDIS_REPLY_NIL) {
        return boost::none;
    }
    return r.get_str();
}

std::vector<std::string> lrange(DBConnector* db, const std::string& key, int start, int stop) {
    std::vector<std::string> result;
    RedisCommand command;
    command.format("LRANGE %s %d %d", key.c_str(), start, stop);
    RedisReply r(db->getRedisContext(db->getDbId()), command, REDIS_REPLY_ARRAY);

    if (r.getContext()->err) {
        SWSS_LOG_ERROR("LRANGE command failed for key %s: %s", key.c_str(), r.getContext()->errstr);
        return result;
    }

    auto reply = r.getContext();
    for (size_t i = 0; i < reply->elements; ++i) {
        if (reply->element[i]->type == REDIS_REPLY_STRING) {
            result.emplace_back(reply->element[i]->str, reply->element[i]->len);
        }
    }
    return result;
}

// Set Commands
long long sadd(DBConnector* db, const std::string& key, const std::string& member) {
    RedisCommand command;
    command.format("SADD %s %s", key.c_str(), member.c_str());
    RedisReply r(db->getRedisContext(db->getDbId()), command, REDIS_REPLY_INTEGER);
    if (r.getContext()->err) {
        SWSS_LOG_ERROR("SADD command failed for key %s member %s: %s", key.c_str(), member.c_str(), r.getContext()->errstr);
        return 0;
    }
    return r.get_longlong();
}

long long srem(DBConnector* db, const std::string& key, const std::string& member) {
    RedisCommand command;
    command.format("SREM %s %s", key.c_str(), member.c_str());
    RedisReply r(db->getRedisContext(db->getDbId()), command, REDIS_REPLY_INTEGER);
    if (r.getContext()->err) {
        SWSS_LOG_ERROR("SREM command failed for key %s member %s: %s", key.c_str(), member.c_str(), r.getContext()->errstr);
        return 0;
    }
    return r.get_longlong();
}

std::vector<std::string> smembers(DBConnector* db, const std::string& key) {
    std::vector<std::string> result;
    RedisCommand command;
    command.format("SMEMBERS %s", key.c_str());
    RedisReply r(db->getRedisContext(db->getDbId()), command, REDIS_REPLY_ARRAY);

    if (r.getContext()->err) {
        SWSS_LOG_ERROR("SMEMBERS command failed for key %s: %s", key.c_str(), r.getContext()->errstr);
        return result;
    }

    auto reply = r.getContext();
    for (size_t i = 0; i < reply->elements; ++i) {
        if (reply->element[i]->type == REDIS_REPLY_STRING) {
            result.emplace_back(reply->element[i]->str, reply->element[i]->len);
        }
    }
    return result;
}

bool sismember(DBConnector* db, const std::string& key, const std::string& member) {
    RedisCommand command;
    command.format("SISMEMBER %s %s", key.c_str(), member.c_str());
    RedisReply r(db->getRedisContext(db->getDbId()), command, REDIS_REPLY_INTEGER);
    if (r.getContext()->err) {
        SWSS_LOG_ERROR("SISMEMBER command failed for key %s member %s: %s", key.c_str(), member.c_str(), r.getContext()->errstr);
        return false;
    }
    return r.get_longlong() == 1;
}

// Sorted Set Commands
long long zadd(DBConnector* db, const std::string& key, double score, const std::string& member) {
    RedisCommand command;
    std::string score_str = std::to_string(score);
    command.format("ZADD %s %s %s", key.c_str(), score_str.c_str(), member.c_str());
    RedisReply r(db->getRedisContext(db->getDbId()), command, REDIS_REPLY_INTEGER);
    if (r.getContext()->err) {
        SWSS_LOG_ERROR("ZADD command failed for key %s score %s member %s: %s", key.c_str(), score_str.c_str(), member.c_str(), r.getContext()->errstr);
        return 0;
    }
    return r.get_longlong();
}

long long zrem(DBConnector* db, const std::string& key, const std::string& member) {
    RedisCommand command;
    command.format("ZREM %s %s", key.c_str(), member.c_str());
    RedisReply r(db->getRedisContext(db->getDbId()), command, REDIS_REPLY_INTEGER);
    if (r.getContext()->err) {
        SWSS_LOG_ERROR("ZREM command failed for key %s member %s: %s", key.c_str(), member.c_str(), r.getContext()->errstr);
        return 0;
    }
    return r.get_longlong();
}

std::vector<std::string> zrange(DBConnector* db, const std::string& key, int start, int stop) {
    std::vector<std::string> result;
    RedisCommand command;
    command.format("ZRANGE %s %d %d", key.c_str(), start, stop);
    RedisReply r(db->getRedisContext(db->getDbId()), command, REDIS_REPLY_ARRAY);

    if (r.getContext()->err) {
        SWSS_LOG_ERROR("ZRANGE command failed for key %s: %s", key.c_str(), r.getContext()->errstr);
        return result;
    }

    auto reply = r.getContext();
    for (size_t i = 0; i < reply->elements; ++i) {
        if (reply->element[i]->type == REDIS_REPLY_STRING) {
            result.emplace_back(reply->element[i]->str, reply->element[i]->len);
        }
    }
    return result;
}

boost::optional<std::string> zscore(DBConnector* db, const std::string& key, const std::string& member) {
    RedisCommand command;
    command.format("ZSCORE %s %s", key.c_str(), member.c_str());
    RedisReply r(db->getRedisContext(db->getDbId()), command, REDIS_REPLY_STRING, true);

    if (r.getContext()->err) {
        SWSS_LOG_ERROR("ZSCORE command failed for key %s member %s: %s", key.c_str(), member.c_str(), r.getContext()->errstr);
        return boost::none;
    }
    if (r.getContext()->type == REDIS_REPLY_NIL) {
        return boost::none;
    }
    return r.get_str();
}

} // namespace swss