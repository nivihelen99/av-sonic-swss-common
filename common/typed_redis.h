#ifndef SWSS_TYPED_REDIS_H
#define SWSS_TYPED_REDIS_H

#include <string>
#include <vector>
#include <optional>
#include <thread>
#include <functional>
#include <memory>
#include <deque>

#include "dbconnector.h"
#include "table.h"
#include "consumerstatetable.h"
#include "select.h"

// Forward declarations for user-provided serialization functions
// These must be implemented by the user of TypedTable for their specific type T
namespace redisutils {
template<typename T>
std::vector<swss::FieldValueTuple> serialize(const T& data);

template<typename T>
std::optional<T> deserialize(const std::vector<swss::FieldValueTuple>& fvs);
} // namespace redisutils

namespace swss {

template<typename T>
class TypedTable {
public:
    TypedTable(const std::string& tableName, const std::string& dbName = "APPL_DB", int dbId = 0, const std::string& host = "localhost", int port = 6379);
    ~TypedTable();

    void set(const std::string& key, const T& data);
    std::optional<T> get(const std::string& key);
    void onChange(std::function<void(const std::string&, const T&)> cb);

private:
    DBConnector m_db;
    Table m_table;
    std::unique_ptr<ConsumerTable> m_consumer;
    std::unique_ptr<Select> m_select;
    std::thread m_thread;
};

// Template implementations must be in the header or included at the end of the header.
// For now, we will define them in common/typed_redis.cpp and include that .cpp file
// at the end of this header. This is a common practice for template definitions.
#include "common/typed_redis.cpp"

} // namespace swss

#endif // SWSS_TYPED_REDIS_H
