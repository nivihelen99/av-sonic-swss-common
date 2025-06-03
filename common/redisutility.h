#pragma once

#include "rediscommand.h" // Assuming this is needed for DBConnector or other utilities

#include <string>
#include <vector>
#include <map>
#include <boost/optional.hpp>
#include <algorithm> // Already present, kept it

namespace swss
{

// Forward declaration
class DBConnector;

boost::optional<std::string> fvsGetValue(
    const std::vector<FieldValueTuple> &fvt,
    const std::string &field,
    bool case_insensitive = false);

// Generic Commands
boost::optional<std::string> genericGet(DBConnector* db, const std::string& key);
void genericSet(DBConnector* db, const std::string& key, const std::string& value); // Consider bool return for success
bool exists(DBConnector* db, const std::string& key);
long long del(DBConnector* db, const std::string& key);

// Hash Commands
boost::optional<std::string> hget(DBConnector* db, const std::string& hash, const std::string& field);
void hset(DBConnector* db, const std::string& hash, const std::string& field, const std::string& value); // Consider bool return
long long hdel(DBConnector* db, const std::string& hash, const std::string& field);
std::map<std::string, std::string> hgetall(DBConnector* db, const std::string& hash);

// List Commands
long long lpush(DBConnector* db, const std::string& key, const std::string& value);
long long rpush(DBConnector* db, const std::string& key, const std::string& value);
boost::optional<std::string> lpop(DBConnector* db, const std::string& key);
boost::optional<std::string> rpop(DBConnector* db, const std::string& key);
std::vector<std::string> lrange(DBConnector* db, const std::string& key, int start, int stop);

// Set Commands
long long sadd(DBConnector* db, const std::string& key, const std::string& member);
long long srem(DBConnector* db, const std::string& key, const std::string& member);
std::vector<std::string> smembers(DBConnector* db, const std::string& key);
bool sismember(DBConnector* db, const std::string& key, const std::string& member);

// Sorted Set Commands
long long zadd(DBConnector* db, const std::string& key, double score, const std::string& member);
long long zrem(DBConnector* db, const std::string& key, const std::string& member);
std::vector<std::string> zrange(DBConnector* db, const std::string& key, int start, int stop); // Consider withscores option
boost::optional<std::string> zscore(DBConnector* db, const std::string& key, const std::string& member);

}
