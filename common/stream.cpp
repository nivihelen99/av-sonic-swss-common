#include "stream.h"
#include "logger.h" // For SWSS_LOG_ERROR, SWSS_LOG_ENTER etc.

namespace swss {

Stream::Stream(DBConnector *db, const std::string &streamName)
    : m_db(db), m_streamName(streamName)
{
    SWSS_LOG_ENTER();
    if (!db)
    {
        SWSS_LOG_ERROR("DBConnector is null for stream %s", m_streamName.c_str());
        throw std::invalid_argument("DBConnector cannot be null.");
    }
}

Stream::~Stream()
{
    SWSS_LOG_ENTER();
}

const std::string& Stream::getName() const
{
    return m_streamName;
}

std::string Stream::xadd(const std::string &id, const std::vector<std::pair<std::string, std::string>> &values, int maxlen, bool approximate)
{
    SWSS_LOG_ENTER();
    return m_db->xadd(m_streamName, id, values, maxlen, approximate);
}

std::shared_ptr<std::vector<std::pair<std::string, std::vector<std::pair<std::string, std::string>>>>> Stream::xread(
    const std::vector<std::string> &ids, 
    int count, 
    int block_ms)
{
    SWSS_LOG_ENTER();
    // This version of xread is for the single stream associated with this Stream object.
    std::vector<std::string> keys = {m_streamName};
    return m_db->xread(keys, ids, count, block_ms);
}

std::shared_ptr<std::vector<std::pair<std::string, std::vector<std::pair<std::string, std::string>>>>> Stream::xread(
    const std::vector<std::string> &keys,
    const std::vector<std::string> &ids, 
    int count, 
    int block_ms)
{
    SWSS_LOG_ENTER();
    // This version allows reading from multiple arbitrary streams.
    // It's less common for a single Stream object instance but provided for completeness.
    return m_db->xread(keys, ids, count, block_ms);
}

bool Stream::xgroup_create(const std::string &groupname, const std::string &id, bool mkstream)
{
    SWSS_LOG_ENTER();
    return m_db->xgroup_create(m_streamName, groupname, id, mkstream);
}

bool Stream::xgroup_destroy(const std::string &groupname)
{
    SWSS_LOG_ENTER();
    return m_db->xgroup_destroy(m_streamName, groupname);
}

int64_t Stream::xgroup_delconsumer(const std::string &groupname, const std::string &consumername)
{
    SWSS_LOG_ENTER();
    return m_db->xgroup_delconsumer(m_streamName, groupname, consumername);
}

std::shared_ptr<std::vector<std::pair<std::string, std::vector<std::pair<std::string, std::string>>>>> Stream::xreadgroup(
    const std::string &groupname, 
    const std::string &consumername, 
    const std::vector<std::string> &ids, 
    int count, 
    bool noack, 
    int block_ms)
{
    SWSS_LOG_ENTER();
    // This version of xreadgroup is for the single stream associated with this Stream object.
    std::vector<std::string> keys = {m_streamName};
    return m_db->xreadgroup(groupname, consumername, keys, ids, count, noack, block_ms);
}

// Overload for xreadgroup for a single stream, single ID (common case)
std::shared_ptr<std::vector<std::pair<std::string, std::vector<std::pair<std::string, std::string>>>>> Stream::xreadgroup(
    const std::string &groupname, 
    const std::string &consumername, 
    const std::string &id, // Default ">"
    int count, 
    bool noack, 
    int block_ms)
{
    SWSS_LOG_ENTER();
    std::vector<std::string> keys = {m_streamName};
    std::vector<std::string> ids = {id};
    return m_db->xreadgroup(groupname, consumername, keys, ids, count, noack, block_ms);
}


int64_t Stream::xack(const std::string &groupname, const std::vector<std::string> &ids)
{
    SWSS_LOG_ENTER();
    return m_db->xack(m_streamName, groupname, ids);
}

std::shared_ptr<RedisReply> Stream::xpending(const std::string &groupname, const std::string &start, const std::string &end, int count, const std::string &consumername)
{
    SWSS_LOG_ENTER();
    return m_db->xpending(m_streamName, groupname, start, end, count, consumername);
}

std::shared_ptr<std::vector<std::pair<std::string, std::vector<std::pair<std::string, std::string>>>>> Stream::xclaim(
    const std::string &groupname, 
    const std::string &consumername, 
    int min_idle_time, 
    const std::vector<std::string> &ids, 
    bool justid)
{
    SWSS_LOG_ENTER();
    return m_db->xclaim(m_streamName, groupname, consumername, min_idle_time, ids, justid);
}

int64_t Stream::xlen()
{
    SWSS_LOG_ENTER();
    return m_db->xlen(m_streamName);
}

int64_t Stream::xtrim(const std::string &strategy, const std::string &threshold, bool approximate, int limit)
{
    SWSS_LOG_ENTER();
    return m_db->xtrim(m_streamName, strategy, threshold, approximate, limit);
}

std::shared_ptr<std::vector<std::pair<std::string, std::vector<std::pair<std::string, std::string>>>>> Stream::xrange(
    const std::string &start, 
    const std::string &end, 
    int count)
{
    SWSS_LOG_ENTER();
    return m_db->xrange(m_streamName, start, end, count);
}

std::shared_ptr<std::vector<std::pair<std::string, std::vector<std::pair<std::string, std::string>>>>> Stream::xrevrange(
    const std::string &end, 
    const std::string &start, 
    int count)
{
    SWSS_LOG_ENTER();
    return m_db->xrevrange(m_streamName, end, start, count);
}

int64_t Stream::xdel(const std::vector<std::string> &ids)
{
    SWSS_LOG_ENTER();
    return m_db->xdel(m_streamName, ids);
}

} // namespace swss
