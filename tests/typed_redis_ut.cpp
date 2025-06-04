#include "gtest/gtest.h"
#include "common/typed_redis.h" // Should include dbconnector.h, table.h etc.
#include "dbconnector.h"
#include "table.h"
#include <future> // For std::promise
#include <chrono> // For timeouts

// Define a sample struct for testing
struct PortInfo {
    std::string adminStatus;
    int mtu;
    bool operUp;

    // Add equality operator for easy comparison in tests
    bool operator==(const PortInfo& other) const {
        return adminStatus == other.adminStatus &&
               mtu == other.mtu &&
               operUp == other.operUp;
    }
    // Add a stream operator for better GTEST failure messages
    friend std::ostream& operator<<(std::ostream& os, const PortInfo& info) {
        os << "PortInfo { adminStatus: \"" << info.adminStatus
           << "\", mtu: " << info.mtu
           << ", operUp: " << (info.operUp ? "true" : "false") << " }";
        return os;
    }
};

// Implement serialize and deserialize for PortInfo in the redisutils namespace
namespace redisutils {

template<>
std::vector<swss::FieldValueTuple> serialize<PortInfo>(const PortInfo& info) {
    return {
        {"admin_status", info.adminStatus},
        {"mtu", std::to_string(info.mtu)},
        {"oper_up", info.operUp ? "true" : "false"}
    };
}

template<>
std::optional<PortInfo> deserialize<PortInfo>(const std::vector<swss::FieldValueTuple>& fvs) {
    PortInfo info;
    bool adminStatusFound = false;
    bool mtuFound = false;
    bool operUpFound = false;

    try {
        if (fvs.empty()) {
            // std::cerr << "Deserialize PortInfo: Empty field-value set." << std::endl;
            return std::nullopt;
        }
        for (const auto& fv : fvs) {
            if (fv.first == "admin_status") {
                info.adminStatus = fv.second;
                adminStatusFound = true;
            } else if (fv.first == "mtu") {
                info.mtu = std::stoi(fv.second);
                mtuFound = true;
            } else if (fv.first == "oper_up") {
                info.operUp = (fv.second == "true");
                operUpFound = true;
            }
        }
        // For this test, let's assume all fields are mandatory for a valid PortInfo during deserialization
        // unless it's a partial update scenario (which TypedTable doesn't explicitly handle for 'get').
        if (!adminStatusFound || !mtuFound || !operUpFound) {
             //std::cerr << "Deserialize PortInfo: Missing one or more mandatory fields." << std::endl;
             //std::cerr << "Found: admin_status=" << adminStatusFound << ", mtu=" << mtuFound << ", oper_up=" << operUpFound << std::endl;
            return std::nullopt;
        }
        return info;
    } catch (const std::invalid_argument& e) {
        // std::cerr << "Deserialization error (invalid_argument) for PortInfo: " << e.what() << std::endl;
        return std::nullopt;
    } catch (const std::out_of_range& e) {
        // std::cerr << "Deserialization error (out_of_range) for PortInfo: " << e.what() << std::endl;
        return std::nullopt;
    } catch (...) {
        // std::cerr << "Unknown deserialization error for PortInfo" << std::endl;
        return std::nullopt;
    }
}
} // namespace redisutils

// Test fixture for TypedTable tests
class TypedTableTest : public ::testing::Test {
protected:
    const std::string testDbName = "APPL_DB"; // Or use a specific test DB like "TEST_DB" if available
    const int testDbId = 0; // Adjust if using a different DB for tests. For sonic-swss this is usually APPL_DB = 0.
    // Using unique table names for each test to avoid interference.
    // Alternatively, clean up keys in TearDown.

    void TearDown() override {
        // General cleanup logic can go here if needed,
        // but individual tests will handle their specific keys for now.
    }

    void clearKey(const std::string& tableName, const std::string& key) {
        swss::DBConnector db(testDbName, testDbId, "localhost", 6379);
        swss::Table t(&db, tableName);
        t.del(key);
    }
};

TEST_F(TypedTableTest, TypedTableSetGet) {
    const std::string tableName = "PORT_TABLE_UT_SETGET";
    swss::TypedTable<PortInfo> portTable(tableName, testDbName, testDbId);

    PortInfo portInfoData;
    portInfoData.adminStatus = "up";
    portInfoData.mtu = 1500;
    portInfoData.operUp = true;

    portTable.set("Ethernet0", portInfoData);

    auto retrievedInfo = portTable.get("Ethernet0");
    ASSERT_TRUE(retrievedInfo.has_value());
    EXPECT_EQ(retrievedInfo.value(), portInfoData);

    clearKey(tableName, "Ethernet0");
}

TEST_F(TypedTableTest, TypedTableGetNotFound) {
    const std::string tableName = "PORT_TABLE_UT_GETNOTFOUND";
    swss::TypedTable<PortInfo> portTable(tableName, testDbName, testDbId);

    auto retrievedInfo = portTable.get("NonExistentKey");
    ASSERT_FALSE(retrievedInfo.has_value());
}

TEST_F(TypedTableTest, TypedTableOnChange) {
    const std::string tableName = "PORT_TABLE_UT_ONCHANGE";
    swss::TypedTable<PortInfo> portTable(tableName, testDbName, testDbId);

    std::promise<std::pair<std::string, PortInfo>> onChangePromise;
    auto onChangeFuture = onChangePromise.get_future();

    portTable.onChange([&](const std::string& key, const PortInfo& info) {
        onChangePromise.set_value({key, info});
    });

    // Give onChange thread a moment to start up and subscribe
    std::this_thread::sleep_for(std::chrono::milliseconds(100));


    PortInfo testData;
    testData.adminStatus = "down";
    testData.mtu = 9000;
    testData.operUp = false;

    // Simulate a Redis update that onChange should pick up
    // Ensure this producer uses the same DB instance details if TypedTable uses non-default ones.
    swss::DBConnector producer_db(testDbName, testDbId, "localhost", 6379);
    swss::Table producerTable(&producer_db, tableName);
    std::vector<swss::FieldValueTuple> fvs = redisutils::serialize<PortInfo>(testData);
    producerTable.set("Ethernet1", fvs);

    auto futureStatus = onChangeFuture.wait_for(std::chrono::seconds(5)); // Increased timeout
    ASSERT_EQ(futureStatus, std::future_status::ready) << "onChange callback was not invoked within timeout";

    auto result = onChangeFuture.get();
    EXPECT_EQ(result.first, "Ethernet1");
    EXPECT_EQ(result.second, testData);

    clearKey(tableName, "Ethernet1");
    // Note: portTable object will be destroyed, joining its thread.
}

TEST_F(TypedTableTest, TypedTableDeserializationErrorGet) {
    const std::string tableName = "PORT_TABLE_UT_DESER_GET";
    const std::string key = "MalformedKeyGet";

    swss::DBConnector db(testDbName, testDbId, "localhost", 6379);
    swss::Table t(&db, tableName);
    std::vector<swss::FieldValueTuple> malformedFvs = {
        {"admin_status", "up"},
        {"mtu", "not_an_int"}, // Malformed
        {"oper_up", "true"}
    };
    t.set(key, malformedFvs);

    swss::TypedTable<PortInfo> portTable(tableName, testDbName, testDbId);
    auto retrievedInfo = portTable.get(key);
    ASSERT_FALSE(retrievedInfo.has_value()) << "Expected nullopt due to deserialization error for key: " << key;

    clearKey(tableName, key);
}

TEST_F(TypedTableTest, TypedTableDeserializationErrorOnChange) {
    const std::string tableName = "PORT_TABLE_UT_DESER_ONCHANGE";
    swss::TypedTable<PortInfo> portTable(tableName, testDbName, testDbId);

    std::promise<void> firstGoodPromise, secondGoodPromise;
    auto firstGoodFuture = firstGoodPromise.get_future();
    auto secondGoodFuture = secondGoodPromise.get_future();

    int callbackCount = 0;

    portTable.onChange([&](const std::string& key, const PortInfo& info) {
        callbackCount++;
        // We only expect callbacks for valid data
        if (key == "EthernetGood1" && info.mtu == 1500) {
            firstGoodPromise.set_value();
        } else if (key == "EthernetGood2" && info.mtu == 9000) {
            secondGoodPromise.set_value();
        } else {
            // This would be an unexpected callback, fail the test.
            // Or if we expect a callback for malformed data (but with nullopt data), adjust here.
            // Current TypedTable implementation filters out deserialization failures before calling cb.
            FAIL() << "Callback received for unexpected key/data or malformed data: " << key;
        }
    });

    std::this_thread::sleep_for(std::chrono::milliseconds(100)); // Wait for onChange setup

    swss::DBConnector producer_db(testDbName, testDbId, "localhost", 6379);
    swss::Table producerTable(&producer_db, tableName);

    // 1. Send good data
    PortInfo goodData1;
    goodData1.adminStatus = "up";
    goodData1.mtu = 1500;
    goodData1.operUp = true;
    producerTable.set("EthernetGood1", redisutils::serialize(goodData1));

    ASSERT_EQ(firstGoodFuture.wait_for(std::chrono::seconds(2)), std::future_status::ready) << "First good data not received";

    // 2. Send malformed data
    std::vector<swss::FieldValueTuple> malformedFvs = {
        {"admin_status", "up"},
        {"mtu", "very_bad_mtu"}, // This will fail deserialization
        {"oper_up", "false"}
    };
    producerTable.set("EthernetMalformed", malformedFvs);

    // 3. Send another piece of good data to ensure the onChange loop continues
    PortInfo goodData2;
    goodData2.adminStatus = "down";
    goodData2.mtu = 9000;
    goodData2.operUp = false;
    producerTable.set("EthernetGood2", redisutils::serialize(goodData2));

    ASSERT_EQ(secondGoodFuture.wait_for(std::chrono::seconds(2)), std::future_status::ready) << "Second good data not received after malformed data";

    EXPECT_EQ(callbackCount, 2) << "Callback count should only be for good messages.";

    clearKey(tableName, "EthernetGood1");
    clearKey(tableName, "EthernetMalformed");
    clearKey(tableName, "EthernetGood2");
}


// It might be good to have a main function for local test execution
// However, for typical build systems like Bazel or CMake, this is not strictly necessary
// as they provide their own main.
// int main(int argc, char **argv) {
// ::testing::InitGoogleTest(&argc, argv);
// return RUN_ALL_TESTS();
// }

// Note: The common/typed_redis.cpp file which contains template implementations
// needs to be handled correctly by the build system.
// If it's compiled separately and linked, explicit template instantiations might be needed
// for PortInfo IF they were not in a header. But since typed_redis.cpp is included by
// typed_redis.h, this should work fine.
//
// For redisutils::serialize and redisutils::deserialize<PortInfo>, they are defined
// in this test file. The linker will pick them up.

// A note on DB state: these tests assume a running Redis instance on localhost:6379.
// They also write to APPL_DB. For a CI environment, a dedicated test Redis instance
// or flushing the DB (e.g., FLUSHDB command) before/after test runs is advisable.
// The current cleanup is per-key which is okay for these specific tests.
// Using unique table names per test case helps isolate tests.

// If swsscommon library is pre-compiled and linked, ensure that the test utilities
// (like Select, DBConnector, Table) are accessible and work as expected.
// The test assumes that these underlying components are functioning correctly.

// The TypedTable destructor waits for the thread to join. If a test fails
// and an exception is thrown, the destructor might not run, or the thread
// might be in a state where it cannot be joined cleanly. GTest handles this
// by running each TEST_F in a separate process or catches exceptions,
// but it's something to be mindful of for complex threaded scenarios.
// The breakSelect() in ~TypedTable helps mitigate this.

// The `onChange` test relies on timing (sleep_for). In a heavily loaded system,
// this might be flaky. A more robust way would be for `onChange` to signal readiness,
// but that adds complexity to the TypedTable API itself. For typical unit tests,
// a short sleep is often acceptable.
// The current 5-second timeout for futures should be generous.

// The deserialize<PortInfo> implementation is now more strict and requires all fields.
// This is a choice for the test; a real application might have different rules.
// Added better error messages for deserialization failures.
// Added operator<< for PortInfo for better GTEST error reporting.

```
