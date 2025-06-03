#include "common/redisutility.h"
#include "common/stringutility.h"
#include "common/boolean.h"
#include "common/dbconnector.h" // Added for DBConnector
#include "common/rediscommand.h" // Added for FLUSHDB

#include <string>
#include <vector>
#include <map>
#include <algorithm> // For std::sort if needed for comparing vectors

#include "gtest/gtest.h"

// Keep the existing test for fvsGetValue
TEST(REDISUTILITY, fvsGetValue)
{
    std::vector<swss::FieldValueTuple> fvt;
    fvt.push_back(std::make_pair("int", "123"));
    fvt.push_back(std::make_pair("bool", "true"));
    fvt.push_back(std::make_pair("string", "name"));

    auto si = swss::fvsGetValue(fvt, "int");
    EXPECT_TRUE(si);
    auto sb = swss::fvsGetValue(fvt, "bool");
    EXPECT_TRUE(sb);
    auto ss = swss::fvsGetValue(fvt, "string");
    EXPECT_TRUE(ss);

    int i;
    swss::AlphaBoolean b;
    std::string s;
    ASSERT_NO_THROW(swss::lexical_convert({*si, *sb, *ss}, i, b, s));
    EXPECT_EQ(i, 123);
    EXPECT_EQ(b, true);
    EXPECT_EQ(s, "name");

    EXPECT_FALSE(swss::fvsGetValue(fvt, "Int"));
    EXPECT_TRUE(swss::fvsGetValue(fvt, "Int", true));
    EXPECT_FALSE(swss::fvsGetValue(fvt, "double"));
}

// Define the Test Fixture
class RedisUtilityTest : public ::testing::Test {
protected:
    std::unique_ptr<swss::DBConnector> m_db;

    RedisUtilityTest() {
        // Constructor can be used if DBConnector initialization is simple enough
        // and doesn't throw exceptions that SetUp would handle better.
    }

    void SetUp() override {
        // Use "APPL_DB" for testing. The last 'true' is for useUnixDomainSocket.
        // Adjust DB name and parameters if necessary for the test environment.
        m_db = std::make_unique<swss::DBConnector>("APPL_DB", 0, true);

        // Flush the database before each test to ensure a clean state
        swss::RedisCommand command;
        command.format("FLUSHDB");
        swss::RedisReply reply(m_db.get(), command);
        // Optionally check reply for errors, though FLUSHDB usually succeeds
        // if the connection is valid.
        ASSERT_FALSE(reply.getContext()->err) << "FLUSHDB failed: " << reply.getContext()->errstr;
    }

    void TearDown() override {
        // m_db will be automatically cleaned up by unique_ptr
        // No specific per-test cleanup needed if FLUSHDB in SetUp is effective.
    }

    // Helper to create unique keys for tests
    std::string testKey(const std::string& baseName) {
        return std::string(::testing::UnitTest::GetInstance()->current_test_info()->test_suite_name())
               + "_" + ::testing::UnitTest::GetInstance()->current_test_info()->name()
               + "_" + baseName;
    }
};

// Test Cases for new Redis Utility Functions

TEST_F(RedisUtilityTest, GenericSetAndGet) {
    std::string key = testKey("mykey");
    std::string value = "myvalue";

    swss::genericSet(m_db.get(), key, value);
    auto result = swss::genericGet(m_db.get(), key);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result.value(), value);

    auto non_existent = swss::genericGet(m_db.get(), testKey("nonexistentkey"));
    EXPECT_FALSE(non_existent.has_value());
}

TEST_F(RedisUtilityTest, Exists) {
    std::string key_exists = testKey("existskey");
    std::string value = "somevalue";
    swss::genericSet(m_db.get(), key_exists, value);

    EXPECT_TRUE(swss::exists(m_db.get(), key_exists));
    EXPECT_FALSE(swss::exists(m_db.get(), testKey("nonexistentkey")));
}

TEST_F(RedisUtilityTest, Del) {
    std::string key_to_del = testKey("deltarget");
    swss::genericSet(m_db.get(), key_to_del, "delete_me");

    EXPECT_EQ(swss::del(m_db.get(), key_to_del), 1);
    EXPECT_FALSE(swss::exists(m_db.get(), key_to_del));
    EXPECT_EQ(swss::del(m_db.get(), testKey("nonexistentkey_del")), 0);
}

TEST_F(RedisUtilityTest, HashCommands) {
    std::string hash = testKey("myhash");
    std::string field1 = "field1";
    std::string value1 = "value1";
    std::string field2 = "field2";
    std::string value2 = "value2";

    // HSET and HGET
    swss::hset(m_db.get(), hash, field1, value1);
    swss::hset(m_db.get(), hash, field2, value2);

    auto hget_res1 = swss::hget(m_db.get(), hash, field1);
    ASSERT_TRUE(hget_res1.has_value());
    EXPECT_EQ(hget_res1.value(), value1);

    auto hget_non_existent_field = swss::hget(m_db.get(), hash, "nonexistentfield");
    EXPECT_FALSE(hget_non_existent_field.has_value());

    auto hget_non_existent_hash = swss::hget(m_db.get(), testKey("nonexistenthash"), field1);
    EXPECT_FALSE(hget_non_existent_hash.has_value());

    // HGETALL
    auto hgetall_res = swss::hgetall(m_db.get(), hash);
    ASSERT_EQ(hgetall_res.size(), 2);
    EXPECT_EQ(hgetall_res[field1], value1);
    EXPECT_EQ(hgetall_res[field2], value2);

    auto hgetall_non_existent = swss::hgetall(m_db.get(), testKey("nonexistenthash_hgetall"));
    EXPECT_TRUE(hgetall_non_existent.empty());

    // HDEL
    EXPECT_EQ(swss::hdel(m_db.get(), hash, field1), 1);
    EXPECT_FALSE(swss::hget(m_db.get(), hash, field1).has_value());
    hgetall_res = swss::hgetall(m_db.get(), hash);
    ASSERT_EQ(hgetall_res.size(), 1);
    EXPECT_EQ(hgetall_res.count(field1), 0);
    EXPECT_EQ(hgetall_res[field2], value2);

    EXPECT_EQ(swss::hdel(m_db.get(), hash, "nonexistentfield_hdel"), 0);
    EXPECT_EQ(swss::hdel(m_db.get(), testKey("nonexistenthash_hdel"), field1), 0);
}

TEST_F(RedisUtilityTest, ListCommands) {
    std::string key = testKey("mylist");
    std::string val1 = "val1";
    std::string val2 = "val2";
    std::string val3 = "val3";

    // LPUSH (val2, val1)
    EXPECT_EQ(swss::lpush(m_db.get(), key, val1), 1);
    EXPECT_EQ(swss::lpush(m_db.get(), key, val2), 2);

    // RPUSH (val2, val1, val3)
    EXPECT_EQ(swss::rpush(m_db.get(), key, val3), 3);

    // LRANGE
    auto lrange_res = swss::lrange(m_db.get(), key, 0, -1);
    ASSERT_EQ(lrange_res.size(), 3);
    EXPECT_EQ(lrange_res[0], val2);
    EXPECT_EQ(lrange_res[1], val1);
    EXPECT_EQ(lrange_res[2], val3);

    auto lrange_subset = swss::lrange(m_db.get(), key, 0, 1);
    ASSERT_EQ(lrange_subset.size(), 2);
    EXPECT_EQ(lrange_subset[0], val2);
    EXPECT_EQ(lrange_subset[1], val1);

    auto lrange_empty = swss::lrange(m_db.get(), testKey("nonexistentlist_lrange"), 0, -1);
    EXPECT_TRUE(lrange_empty.empty());

    // LPOP
    auto lpop_res = swss::lpop(m_db.get(), key);
    ASSERT_TRUE(lpop_res.has_value());
    EXPECT_EQ(lpop_res.value(), val2); // val2 was LPUSHed last among LPUSHes (so it's first)

    // RPOP
    auto rpop_res = swss::rpop(m_db.get(), key);
    ASSERT_TRUE(rpop_res.has_value());
    EXPECT_EQ(rpop_res.value(), val3); // val3 was RPUSHed last

    // Check remaining element
    lrange_res = swss::lrange(m_db.get(), key, 0, -1);
    ASSERT_EQ(lrange_res.size(), 1);
    EXPECT_EQ(lrange_res[0], val1);

    // Pop last element
    EXPECT_EQ(swss::lpop(m_db.get(), key).value(), val1);
    EXPECT_FALSE(swss::lpop(m_db.get(), key).has_value()); // List is now empty
    EXPECT_FALSE(swss::rpop(m_db.get(), key).has_value()); // List is now empty

    EXPECT_FALSE(swss::lpop(m_db.get(), testKey("nonexistentlist_lpop")).has_value());
}

TEST_F(RedisUtilityTest, SetCommands) {
    std::string key = testKey("myset");
    std::string member1 = "member1";
    std::string member2 = "member2";
    std::string member3 = "member3";

    // SADD
    EXPECT_EQ(swss::sadd(m_db.get(), key, member1), 1);
    EXPECT_EQ(swss::sadd(m_db.get(), key, member2), 1);
    EXPECT_EQ(swss::sadd(m_db.get(), key, member1), 0); // Duplicate

    // SISMEMBER
    EXPECT_TRUE(swss::sismember(m_db.get(), key, member1));
    EXPECT_FALSE(swss::sismember(m_db.get(), key, member3));
    EXPECT_FALSE(swss::sismember(m_db.get(), testKey("nonexistentset_sismember"), member1));

    // SMEMBERS
    auto smembers_res = swss::smembers(m_db.get(), key);
    ASSERT_EQ(smembers_res.size(), 2);
    // Order is not guaranteed in sets, so sort before comparing or check presence
    std::sort(smembers_res.begin(), smembers_res.end());
    EXPECT_EQ(smembers_res[0], member1);
    EXPECT_EQ(smembers_res[1], member2);

    auto smembers_non_existent = swss::smembers(m_db.get(), testKey("nonexistentset_smembers"));
    EXPECT_TRUE(smembers_non_existent.empty());

    // SREM
    EXPECT_EQ(swss::srem(m_db.get(), key, member1), 1);
    EXPECT_FALSE(swss::sismember(m_db.get(), key, member1));
    EXPECT_EQ(swss::srem(m_db.get(), key, member3), 0); // Non-existent member
    EXPECT_EQ(swss::srem(m_db.get(), testKey("nonexistentset_srem"), member1), 0);

    smembers_res = swss::smembers(m_db.get(), key);
    ASSERT_EQ(smembers_res.size(), 1);
    EXPECT_EQ(smembers_res[0], member2);
}

TEST_F(RedisUtilityTest, SortedSetCommands) {
    std::string key = testKey("myzset");
    std::string member1 = "member1";
    double score1 = 10.0;
    std::string member2 = "member2";
    double score2 = 20.0;
    std::string member3 = "member3";
    // double score3 = 5.0; // For testing order later

    // ZADD
    EXPECT_EQ(swss::zadd(m_db.get(), key, score1, member1), 1);
    EXPECT_EQ(swss::zadd(m_db.get(), key, score2, member2), 1);
    EXPECT_EQ(swss::zadd(m_db.get(), key, score1 + 1, member1), 0); // Update score for member1

    // ZSCORE
    auto zscore_res1 = swss::zscore(m_db.get(), key, member1);
    ASSERT_TRUE(zscore_res1.has_value());
    // Score is string, convert for comparison. std::stod for string to double.
    EXPECT_DOUBLE_EQ(std::stod(zscore_res1.value()), score1 + 1);

    EXPECT_FALSE(swss::zscore(m_db.get(), key, member3).has_value());
    EXPECT_FALSE(swss::zscore(m_db.get(), testKey("nonexistentzset_zscore"), member1).has_value());

    // ZRANGE (member1 score updated to 11, member2 score 20)
    // Order should be member1, member2
    swss::zadd(m_db.get(), key, 5.0, member3); // Add member3 with score 5.0
                                                 // Expected order: member3, member1, member2

    auto zrange_res = swss::zrange(m_db.get(), key, 0, -1);
    ASSERT_EQ(zrange_res.size(), 3);
    EXPECT_EQ(zrange_res[0], member3); // score 5.0
    EXPECT_EQ(zrange_res[1], member1); // score 11.0
    EXPECT_EQ(zrange_res[2], member2); // score 20.0

    auto zrange_subset = swss::zrange(m_db.get(), key, 0, 1);
    ASSERT_EQ(zrange_subset.size(), 2);
    EXPECT_EQ(zrange_subset[0], member3);
    EXPECT_EQ(zrange_subset[1], member1);

    auto zrange_non_existent = swss::zrange(m_db.get(), testKey("nonexistentzset_zrange"), 0, -1);
    EXPECT_TRUE(zrange_non_existent.empty());

    // ZREM
    EXPECT_EQ(swss::zrem(m_db.get(), key, member1), 1);
    EXPECT_FALSE(swss::zscore(m_db.get(), key, member1).has_value());
    EXPECT_EQ(swss::zrem(m_db.get(), key, "nonexistentmember_zrem"), 0);
    EXPECT_EQ(swss::zrem(m_db.get(), testKey("nonexistentzset_zrem"), member1), 0);

    zrange_res = swss::zrange(m_db.get(), key, 0, -1);
    ASSERT_EQ(zrange_res.size(), 2); // member3, member2 remaining
    EXPECT_EQ(zrange_res[0], member3);
    EXPECT_EQ(zrange_res[1], member2);
}
