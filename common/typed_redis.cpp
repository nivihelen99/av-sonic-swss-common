#include "common/typed_redis.h"
#include <iostream> // For logging errors, replace with a proper logger if available

// Note: This file is included by common/typed_redis.h for template instantiation.
// Do not compile this file separately.

namespace swss {

template<typename T>
TypedTable<T>::TypedTable(const std::string& tableName, const std::string& dbName, int dbId, const std::string& host, int port)
    : m_db(dbName, dbId, host, port), m_table(&m_db, tableName)
{
}

template<typename T>
TypedTable<T>::~TypedTable()
{
    if (m_thread.joinable())
    {
        // Ideally, we need a way to signal the thread to stop.
        // For now, we'll just detach if it's problematic to join,
        // or rely on program termination to clean up.
        // A better solution would involve a stopping condition for the select loop.
        // For simplicity in this example, we join.
        // Consider adding a m_stop_event or similar mechanism.
        m_select->breakSelect(1000); // Break the select loop with a timeout
        m_thread.join();
    }
}

template<typename T>
void TypedTable<T>::set(const std::string& key, const T& data)
{
    std::vector<FieldValueTuple> fvs = redisutils::serialize<T>(data);
    m_table.set(key, fvs);
}

template<typename T>
std::optional<T> TypedTable<T>::get(const std::string& key)
{
    std::vector<FieldValueTuple> fvs;
    if (!m_table.get(key, fvs))
    {
        return std::nullopt;
    }
    return redisutils::deserialize<T>(fvs);
}

template<typename T>
void TypedTable<T>::onChange(std::function<void(const std::string&, const T&)> cb)
{
    m_consumer = std::make_unique<ConsumerTable>(&m_db, m_table.getTableName());
    m_select = std::make_unique<Select>();
    m_select->addSelectable(m_consumer.get());

    m_thread = std::thread([this, cb]() {
        while (true) // Add a condition to stop the thread gracefully
        {
            Selectable *sel;
            int ret = m_select->select(&sel, 1000); // Timeout of 1 second

            if (ret == Select::OBJECT)
            {
                std::deque<KeyOpFieldsValuesTuple> entries;
                m_consumer->pops(entries);

                for (const auto& entry : entries)
                {
                    const std::string& key = kfvKey(entry);
                    // const std::string& op = kfvOp(entry); // Operation can be used if needed
                    const std::vector<FieldValueTuple>& fvs = kfvFieldsValues(entry);

                    try
                    {
                        std::optional<T> parsed_data = redisutils::deserialize<T>(fvs);
                        if (parsed_data)
                        {
                            cb(key, *parsed_data);
                        }
                        else
                        {
                            // Log deserialization failure if needed
                            // std::cerr << "Failed to deserialize data for key: " << key << std::endl;
                        }
                    }
                    catch (const std::exception& e)
                    {
                        // Log exception during deserialization
                        // std::cerr << "Exception during deserialization for key " << key << ": " << e.what() << std::endl;
                        // Depending on error handling strategy, you might want to continue or handle differently.
                    }
                }
            }
            else if (ret == Select::TIMEOUT)
            {
                // Handle timeout, possibly check for a stop condition
            }
            else if (ret == Select::ERROR)
            {
                // Handle error, log and possibly break or retry
                // std::cerr << "Select error." << std::endl;
                break; // Exit on error for now
            }
            else if (ret == Select::SELECT_BREAK)
            {
                // select was broken by another thread calling breakSelect
                break;
            }
            // Add a check for a stop condition here to allow graceful shutdown
            // For example, if (m_should_stop) break;
        }
    });
}

} // namespace swss
