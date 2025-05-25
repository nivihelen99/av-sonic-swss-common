#ifndef __STREAM__
#define __STREAM__

#include <string>
#include <vector>
#include <utility> // For std::pair
#include <memory>  // For std::shared_ptr

#include "dbconnector.h"
#include "redisreply.h" // For RedisReply if needed for XPENDING

namespace swss {

class Stream
{
public:
    Stream(DBConnector *db, const std::string &streamName);
    virtual ~Stream();

    // Stream Commands
    virtual std::string xadd(const std::string &id, const std::vector<std::pair<std::string, std::string>> &values, int maxlen = 0, bool approximate = false);
    
    // Returns: A shared_ptr to a vector of pairs. Each outer pair contains:
    //          - string: The stream key (name).
    //          - vector<pair<string, string>>: A vector of messages. Each inner pair contains:
    //              - string: The message ID.
    //              - string: A string concatenating "field1:value1,field2:value2,..." for that message.
    // Returns nullptr if no messages are read (e.g., on timeout for blocking reads).
    virtual std::shared_ptr<std::vector<std::pair<std::string, std::vector<std::pair<std::string, std::string>>>>> xread(
        const std::vector<std::string> &ids, 
        int count = -1, 
        int block_ms = -1);

    // Overload for reading from multiple streams (keys) - less common for a single Stream object instance
    // but provided for completeness if this class were to manage multiple stream keys, which it currently doesn't.
    // For now, the primary xread will use the Stream's m_streamName.
    virtual std::shared_ptr<std::vector<std::pair<std::string, std::vector<std::pair<std::string, std::string>>>>> xread(
        const std::vector<std::string> &keys,
        const std::vector<std::string> &ids, 
        int count = -1, 
        int block_ms = -1);

    virtual bool xgroup_create(const std::string &groupname, const std::string &id = "$", bool mkstream = false);
    virtual bool xgroup_destroy(const std::string &groupname);
    virtual int64_t xgroup_delconsumer(const std::string &groupname, const std::string &consumername);

    // Returns: Similar structure to xread.
    // Returns nullptr if no messages are read.
    virtual std::shared_ptr<std::vector<std::pair<std::string, std::vector<std::pair<std::string, std::string>>>>> xreadgroup(
        const std::string &groupname, 
        const std::string &consumername, 
        const std::vector<std::string> &ids, 
        int count = -1, 
        bool noack = false, 
        int block_ms = -1);
    
    // Overload for xreadgroup for a single stream (the common case for this class)
    virtual std::shared_ptr<std::vector<std::pair<std::string, std::vector<std::pair<std::string, std::string>>>>> xreadgroup(
        const std::string &groupname, 
        const std::string &consumername, 
        const std::string &id = ">", // Special ID for unread messages
        int count = -1, 
        bool noack = false, 
        int block_ms = -1);


    virtual int64_t xack(const std::string &groupname, const std::vector<std::string> &ids);
    
    // Returns a RedisReply object directly as XPENDING has a complex reply structure.
    virtual std::shared_ptr<RedisReply> xpending(const std::string &groupname, const std::string &start = "-", const std::string &end = "+", int count = -1, const std::string &consumername = "");

    // Returns: Similar structure to xread.
    // Returns an empty vector if no messages are claimed or if ids vector is empty.
    virtual std::shared_ptr<std::vector<std::pair<std::string, std::vector<std::pair<std::string, std::string>>>>> xclaim(
        const std::string &groupname, 
        const std::string &consumername, 
        int min_idle_time, 
        const std::vector<std::string> &ids, 
        bool justid = false);

    virtual int64_t xlen();
    virtual int64_t xtrim(const std::string &strategy, const std::string &threshold, bool approximate = false, int limit = 0);

    // Returns: Similar structure to xread.
    virtual std::shared_ptr<std::vector<std::pair<std::string, std::vector<std::pair<std::string, std::string>>>>> xrange(
        const std::string &start, 
        const std::string &end, 
        int count = -1);

    // Returns: Similar structure to xread.
    virtual std::shared_ptr<std::vector<std::pair<std::string, std::vector<std::pair<std::string, std::string>>>>> xrevrange(
        const std::string &end, 
        const std::string &start, 
        int count = -1);
        
    virtual int64_t xdel(const std::vector<std::string> &ids);

    const std::string& getName() const;

protected:
    DBConnector *m_db;
    std::string m_streamName;
    std::string m_shaXread; // For potential future LUA script usage
};

} // namespace swss

#endif // __STREAM__
