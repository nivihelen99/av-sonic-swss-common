#include "common/reconciler.h"
#include "gtest/gtest.h" // Include gtest header
#include <string>
#include <vector>
#include <map>
#include <unordered_map>
#include <set>

// Helper to allow gtest to print std::set
namespace testing {
namespace internal {
template <typename T>
::std::ostream& PrintTo(const std::set<T>& s, ::std::ostream* os) {
    *os << "{ ";
    bool first = true;
    for (const auto& item : s) {
        if (!first) *os << ", ";
        *os << PrintToString(item);
        first = false;
    }
    *os << " }"; // Added space before closing brace for consistency
    return *os;
}

// Helper to allow gtest to print std::map (needed for UpdatedMapType if it's std::map)
template <typename K, typename V>
::std::ostream& PrintTo(const std::map<K, V>& m, ::std::ostream* os) {
    *os << "{ ";
    bool first = true;
    for (const auto& pair_item : m) { // Using pair_item to avoid structured binding issues in older gtest/compilers if any
        if (!first) *os << ", ";
        *os << PrintToString(pair_item.first) << ":" << PrintToString(pair_item.second);
        first = false;
    }
    *os << " }";
    return *os;
}

// Helper to allow gtest to print std::unordered_map (needed for UpdatedMapType if it's std::unordered_map)
template <typename K, typename V>
::std::ostream& PrintTo(const std::unordered_map<K, V>& m, ::std::ostream* os) {
    *os << "{ ";
    bool first = true;
    // Note: Order is not guaranteed. For stable test output, consider sorting keys if possible,
    // or use gmock_unordered_elements_are for matching if order doesn't matter but elements do.
    // This printer is primarily for informative messages.
    std::vector<K> keys;
    for(const auto& pair_item : m) keys.push_back(pair_item.first);
    // Sort keys to make output deterministic for unordered_map, if keys are sortable.
    // This requires K to have operator<. If not, the output order remains non-deterministic.
    // For simplicity here, we'll iterate as is, acknowledging non-deterministic order.
    // If K is not sortable, the following sort line would fail.
    // if constexpr (has_less_than_operator_v<K>) { std::sort(keys.begin(), keys.end()); }

    for (const auto& pair_item : m) { // Iterate directly for printing
        if (!first) *os << ", ";
        *os << PrintToString(pair_item.first) << ":" << PrintToString(pair_item.second);
        first = false;
    }
    *os << " } (order not guaranteed)";
    return *os;
}
} // namespace internal
} // namespace testing


// Define NonHashableKey and NonHashableValue globally
struct NonHashableKey {
    int id;
    std::string data;

    bool operator<(const NonHashableKey& other) const {
        if (id != other.id) return id < other.id;
        return data < other.data;
    }
    bool operator==(const NonHashableKey& other) const {
        return id == other.id && data == other.data;
    }
    // GTest needs this for printing if not specialized with PrintTo
    friend std::ostream& operator<<(std::ostream& os, const NonHashableKey& k) {
        return os << "NHKey(" << k.id << ", \"" << k.data << "\")";
    }
};


struct NonHashableValue {
    std::string value_data;
    int value_metric;

    bool operator!=(const NonHashableValue& other) const {
        return value_data != other.value_data || value_metric != other.value_metric;
    }
    bool operator==(const NonHashableValue& other) const {
        return value_data == other.value_data && value_metric == other.value_metric;
    }
    // GTest needs this for printing if not specialized with PrintTo
    friend std::ostream& operator<<(std::ostream& os, const NonHashableValue& v) {
        return os << "NHValue(\"" << v.value_data << "\", " << v.value_metric << ")";
    }
};

// Test Fixture for Reconciler tests
class ReconcilerTest : public ::testing::Test {
protected:
    // Common setup/teardown or helper methods can go here
};

TEST_F(ReconcilerTest, HashableKeys_BasicOperations) {
    using Reconciler = aos_utils::Reconciler<std::string, int>;
    using Delta = Reconciler::Delta;

    std::unordered_map<std::string, int> desired = {{"apple", 1}, {"banana", 2}, {"cherry", 3}};
    std::unordered_map<std::string, int> actual = {{"banana", 20}, {"cherry", 3}, {"date", 4}};

    Delta d = Reconciler::computeDelta(desired, actual);

    EXPECT_EQ(d.added, std::set<std::string>({"apple"}));
    EXPECT_EQ(d.removed, std::set<std::string>({"date"}));

    // For updated, check size and then content
    ASSERT_EQ(d.updated.size(), 1);
    // Use .count to ensure key exists before using .at() for unordered_map
    ASSERT_EQ(d.updated.count("banana"), 1);
    EXPECT_EQ(d.updated.at("banana"), 2); // desired value is stored
}

TEST_F(ReconcilerTest, HashableKeys_VectorInput) {
    using Reconciler = aos_utils::Reconciler<std::string, int>;
    using Delta = Reconciler::Delta;

    std::vector<std::pair<std::string, int>> desired_vec = {{"one", 1}, {"two", 2}};
    std::vector<std::pair<std::string, int>> actual_vec = {{"two", 22}, {"three", 3}};

    Delta d = Reconciler::computeDelta(desired_vec, actual_vec);

    EXPECT_EQ(d.added, std::set<std::string>({"one"}));
    EXPECT_EQ(d.removed, std::set<std::string>({"three"}));
    ASSERT_EQ(d.updated.size(), 1);
    ASSERT_EQ(d.updated.count("two"), 1);
    EXPECT_EQ(d.updated.at("two"), 2);
}

TEST_F(ReconcilerTest, NonHashableKeys_BasicOperations) {
    using Reconciler = aos_utils::Reconciler<NonHashableKey, NonHashableValue>;
    using Delta = Reconciler::Delta;

    std::map<NonHashableKey, NonHashableValue> desired = {
        {{1, "a"}, {"val_a", 10}},
        {{2, "b"}, {"val_b", 20}}
    };
    std::map<NonHashableKey, NonHashableValue> actual = {
        {{2, "b"}, {"val_b_changed", 22}},
        {{3, "c"}, {"val_c", 30}}
    };

    Delta d = Reconciler::computeDelta(desired, actual);

    NonHashableKey key1a = {1, "a"};
    NonHashableKey key2b = {2, "b"};
    NonHashableKey key3c = {3, "c"};
    NonHashableValue val_b_20 = {"val_b", 20};

    EXPECT_EQ(d.added, std::set<NonHashableKey>({key1a}));
    EXPECT_EQ(d.removed, std::set<NonHashableKey>({key3c}));
    ASSERT_EQ(d.updated.size(), 1);
    // For std::map, .at() is fine if key is expected.
    EXPECT_EQ(d.updated.at(key2b), val_b_20);
}

TEST_F(ReconcilerTest, EmptyInputs) {
    using ReconcilerHashable = aos_utils::Reconciler<std::string, int>;
    using DeltaHashable = ReconcilerHashable::Delta;
    using ReconcilerNonHashable = aos_utils::Reconciler<NonHashableKey, NonHashableValue>;
    using DeltaNonHashable = ReconcilerNonHashable::Delta;

    std::unordered_map<std::string, int> actual_hashable_with_items = {{"a",1}};
    std::map<NonHashableKey, NonHashableValue> actual_nonhashable_with_items = {{{1,"a"}, {"val",1}}};

    std::unordered_map<std::string, int> empty_hashable_map;
    std::vector<std::pair<std::string, int>> empty_hashable_vec; // Used as an empty container

    std::map<NonHashableKey, NonHashableValue> empty_nonhashable_map;
    std::vector<std::pair<NonHashableKey, NonHashableValue>> empty_nonhashable_vec; // Used as an empty container, corrected Key type


    // Empty desired, actual has items
    DeltaHashable d_empty_desired_h = ReconcilerHashable::computeDelta(empty_hashable_map, actual_hashable_with_items);
    EXPECT_TRUE(d_empty_desired_h.added.empty());
    EXPECT_EQ(d_empty_desired_h.removed, std::set<std::string>({"a"}));
    EXPECT_TRUE(d_empty_desired_h.updated.empty());

    DeltaNonHashable d_empty_desired_nh = ReconcilerNonHashable::computeDelta(empty_nonhashable_map, actual_nonhashable_with_items);
    EXPECT_TRUE(d_empty_desired_nh.added.empty());
    EXPECT_EQ(d_empty_desired_nh.removed, std::set<NonHashableKey>({{1,"a"}}));
    EXPECT_TRUE(d_empty_desired_nh.updated.empty());

    // Desired has items, empty actual
    DeltaHashable d_empty_actual_h = ReconcilerHashable::computeDelta(actual_hashable_with_items, empty_hashable_vec);
    EXPECT_EQ(d_empty_actual_h.added, std::set<std::string>({"a"}));
    EXPECT_TRUE(d_empty_actual_h.removed.empty());
    EXPECT_TRUE(d_empty_actual_h.updated.empty());

    DeltaNonHashable d_empty_actual_nh = ReconcilerNonHashable::computeDelta(actual_nonhashable_with_items, empty_nonhashable_vec);
    EXPECT_EQ(d_empty_actual_nh.added, std::set<NonHashableKey>({{1,"a"}}));
    EXPECT_TRUE(d_empty_actual_nh.removed.empty());
    EXPECT_TRUE(d_empty_actual_nh.updated.empty());

    // Both empty
    DeltaHashable d_both_empty_h = ReconcilerHashable::computeDelta(empty_hashable_map, empty_hashable_map);
    EXPECT_TRUE(d_both_empty_h.added.empty());
    EXPECT_TRUE(d_both_empty_h.removed.empty());
    EXPECT_TRUE(d_both_empty_h.updated.empty());

    DeltaNonHashable d_both_empty_nh = ReconcilerNonHashable::computeDelta(empty_nonhashable_map, empty_nonhashable_map); // can also use empty_nonhashable_vec
    EXPECT_TRUE(d_both_empty_nh.added.empty());
    EXPECT_TRUE(d_both_empty_nh.removed.empty());
    EXPECT_TRUE(d_both_empty_nh.updated.empty());
}

TEST_F(ReconcilerTest, ValueEqualityCheck) {
    using Reconciler = aos_utils::Reconciler<int, std::string>;
    using Delta = Reconciler::Delta;

    std::map<int, std::string> desired = {{1, "hello"}};
    std::map<int, std::string> actual_same_value = {{1, "hello"}};
    std::map<int, std::string> actual_diff_value = {{1, "world"}};

    // Same value - no changes expected
    Delta d_same = Reconciler::computeDelta(desired, actual_same_value);
    EXPECT_TRUE(d_same.added.empty());
    EXPECT_TRUE(d_same.removed.empty());
    EXPECT_TRUE(d_same.updated.empty());

    // Different value - updated expected
    Delta d_diff = Reconciler::computeDelta(desired, actual_diff_value);
    EXPECT_TRUE(d_diff.added.empty());
    EXPECT_TRUE(d_diff.removed.empty());
    ASSERT_EQ(d_diff.updated.size(), 1);
    // For std::map, .at() is fine if key is expected.
    EXPECT_EQ(d_diff.updated.at(1), "hello"); // desired value stored
}
// No main() function needed, gtest provides it.
// Note: Made NonHashableKey and NonHashableValue ostream operators friend functions inside the struct
// for better encapsulation and to ensure GTest can find them.
// Corrected PrintTo for std::set to add a space before the closing brace.
// Used pair_item in PrintTo for maps to avoid potential structured binding issues with older gtest/compilers.
// Added ASSERT_EQ(d.updated.count("key"), 1) before d.updated.at("key") for unordered_map access in tests.
// Corrected `empty_nonhashable_vec` to use `NonHashableKey` not `const NonHashableKey` for its pair elements.
// Changed variable names in EmptyInputs test from `desired_hashable` to `actual_hashable_with_items` etc. for clarity.The file `tests/reconciler_ut.cpp` has been successfully overwritten with the Google Test-based implementation.
The new code incorporates:
-   `gtest/gtest.h` inclusion.
-   `TEST_F` macros for test definitions.
-   Gtest assertions (`EXPECT_EQ`, `ASSERT_EQ`, `EXPECT_TRUE`).
-   `testing::internal::PrintTo` specializations for `std::set`, `std::map`, and `std::unordered_map`.
-   Global definitions for `NonHashableKey` and `NonHashableValue` with friend `operator<<` overloads for GTest printing.
-   Robust checks for `std::unordered_map::at` by using `count` first.
-   The `main()` function from the previous version has been removed as GTest provides its own.

The unit tests now integrate with the Google Test framework. The next logical step is to submit the subtask report.
