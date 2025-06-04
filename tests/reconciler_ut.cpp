#include "common/reconciler.h"
#include <iostream>
#include <string>
#include <vector>
#include <cassert> // For assert

// Helper to print sets for debugging
template <typename T>
std::ostream& operator<<(std::ostream& os, const std::set<T>& s) {
    os << "{ ";
    for (const auto& item : s) {
        os << item << " ";
    }
    os << "}";
    return os;
}

// Helper to print maps for debugging
template <typename K, typename V>
std::ostream& operator<<(std::ostream& os, const std::map<K, V>& m) {
    os << "{ ";
    for (const auto& pair_item : m) { // Changed to avoid structured binding for broader compatibility potentially
        os << pair_item.first << ":" << pair_item.second << " ";
    }
    os << "}";
    return os;
}

template <typename K, typename V>
std::ostream& operator<<(std::ostream& os, const std::unordered_map<K, V>& m) {
    os << "{ ";
    for (const auto& pair_item : m) { // Changed to avoid structured binding
        os << pair_item.first << ":" << pair_item.second << " ";
    }
    os << "}";
    return os;
}


// Test case for hashable keys (e.g., int, std::string)
void test_hashable_keys() {
    std::cout << "--- Test Case: Hashable Keys (std::string, int) ---" << std::endl;
    using Reconciler = aos_utils::Reconciler<std::string, int>;
    using Delta = Reconciler::Delta;

    std::unordered_map<std::string, int> desired1 = {{"apple", 1}, {"banana", 2}, {"cherry", 3}};
    std::unordered_map<std::string, int> actual1 = {{"banana", 20}, {"cherry", 3}, {"date", 4}};

    Delta d1 = Reconciler::computeDelta(desired1, actual1);
    std::cout << "Desired: " << desired1 << std::endl;
    std::cout << "Actual: " << actual1 << std::endl;
    std::cout << "Added: " << d1.added << ", Expected: { apple }" << std::endl;
    std::cout << "Removed: " << d1.removed << ", Expected: { date }" << std::endl;
    std::cout << "Updated: " << d1.updated << ", Expected: { banana:2 }" << std::endl;

    assert(d1.added == std::set<std::string>({"apple"}));
    assert(d1.removed == std::set<std::string>({"date"}));
    assert(d1.updated.size() == 1 && d1.updated.at("banana") == 2);

    // Test with vector of pairs
    // Using std::pair<std::string, int> for vector elements for direct compatibility with unordered_map iteration
    std::vector<std::pair<std::string, int>> desired2_vec = {{"one", 1}, {"two", 2}};
    std::vector<std::pair<std::string, int>> actual2_vec = {{"two", 22}, {"three", 3}};

    Delta d2 = Reconciler::computeDelta(desired2_vec, actual2_vec);
    // Manual print for vector as it doesn't have a generic ostream operator here
    std::cout << "Desired Vec: { one:1 two:2 }" << std::endl;
    std::cout << "Actual Vec: { two:22 three:3 }" << std::endl;
    std::cout << "Added: " << d2.added << ", Expected: { one }" << std::endl;
    std::cout << "Removed: " << d2.removed << ", Expected: { three }" << std::endl;
    std::cout << "Updated: " << d2.updated << ", Expected: { two:2 }" << std::endl;
    assert(d2.added == std::set<std::string>({"one"}));
    assert(d2.removed == std::set<std::string>({"three"}));
    assert(d2.updated.size() == 1 && d2.updated.at("two") == 2);

    std::cout << "Hashable keys test passed." << std::endl;
}

// A simple non-hashable key type for testing std::map path
struct NonHashableKey {
    int id;
    std::string data;

    bool operator<(const NonHashableKey& other) const {
        if (id != other.id) return id < other.id;
        return data < other.data; // Ensure consistent ordering
    }
    // Required for std::map key comparison if used directly and for std::set
    bool operator==(const NonHashableKey& other) const {
        return id == other.id && data == other.data;
    }
    // Required for std::unordered_map if we were to try and hash it, but we are not.
    // For std::map, operator< is sufficient for ordering.
    // operator== is good practice for keys.
};

// Need to provide an ostream operator for NonHashableKey to use it in std::set output
std::ostream& operator<<(std::ostream& os, const NonHashableKey& k) {
    os << "NHKey(" << k.id << ", \"" << k.data << "\")"; // Added quotes for data
    return os;
}

// Value type for NonHashableKey test, needs operator!=
struct NonHashableValue {
    std::string value_data;
    int value_metric;

    bool operator!=(const NonHashableValue& other) const {
        return value_data != other.value_data || value_metric != other.value_metric;
    }
    // operator== is good practice though not strictly required if != is defined
    bool operator==(const NonHashableValue& other) const {
        return value_data == other.value_data && value_metric == other.value_metric;
    }
};

std::ostream& operator<<(std::ostream& os, const NonHashableValue& v) {
    os << "NHValue(\"" << v.value_data << "\", " << v.value_metric << ")";
    return os;
}


void test_non_hashable_keys() {
    std::cout << "--- Test Case: Non-Hashable Keys (Custom Struct) ---" << std::endl;
    using Reconciler = aos_utils::Reconciler<NonHashableKey, NonHashableValue>; // Changed Value to NonHashableValue
    using Delta = Reconciler::Delta;

    std::map<NonHashableKey, NonHashableValue> desired1 = {
        {{1, "a"}, {"val_a", 10}},
        {{2, "b"}, {"val_b", 20}}
    };
    std::map<NonHashableKey, NonHashableValue> actual1 = {
        {{2, "b"}, {"val_b_changed", 22}}, // Value changed
        {{3, "c"}, {"val_c", 30}}       // New item in actual
    };

    Delta d1 = Reconciler::computeDelta(desired1, actual1);
    std::cout << "Desired: " << desired1 << std::endl;
    std::cout << "Actual: " << actual1 << std::endl;
    std::cout << "Added: " << d1.added << ", Expected: { NHKey(1, \"a\") }" << std::endl;
    std::cout << "Removed: " << d1.removed << ", Expected: { NHKey(3, \"c\") }" << std::endl;
    std::cout << "Updated: " << d1.updated << ", Expected: { NHKey(2, \"b\"):NHValue(\"val_b\", 20) }" << std::endl;

    assert(d1.added.size() == 1 && d1.added.count({1, "a"}));
    assert(d1.removed.size() == 1 && d1.removed.count({3, "c"}));
    assert(d1.updated.size() == 1 && d1.updated.at({2, "b"}) == NonHashableValue{"val_b", 20});


    // Test with empty inputs
    std::map<NonHashableKey, NonHashableValue> empty_map;
    // Using a vector of pairs for actual2_vec that can be converted to a map
    std::vector<std::pair<const NonHashableKey, NonHashableValue>> empty_vec;


    Delta d_empty_desired = Reconciler::computeDelta(empty_map, actual1);
    assert(d_empty_desired.added.empty());
    assert(d_empty_desired.removed.size() == 2);
    assert(d_empty_desired.updated.empty());

    Delta d_empty_actual = Reconciler::computeDelta(desired1, empty_vec); // empty_vec instead of empty_map
    assert(d_empty_actual.added.size() == 2);
    assert(d_empty_actual.removed.empty());
    assert(d_empty_actual.updated.empty());

    Delta d_both_empty = Reconciler::computeDelta(empty_map, empty_map);
    assert(d_both_empty.added.empty());
    assert(d_both_empty.removed.empty());
    assert(d_both_empty.updated.empty());

    std::cout << "Non-hashable keys test passed." << std::endl;
}

void test_value_equality() {
    std::cout << "--- Test Case: Value Equality ---" << std::endl;
    using Reconciler = aos_utils::Reconciler<int, std::string>; // Using simple types for clarity
    using Delta = Reconciler::Delta;

    std::map<int, std::string> desired = {{1, "hello"}}; // Using std::map for this test, could be unordered_map
    std::map<int, std::string> actual_same_value = {{1, "hello"}};
    std::map<int, std::string> actual_diff_value = {{1, "world"}};

    Delta d_same = Reconciler::computeDelta(desired, actual_same_value);
    std::cout << "Desired: " << desired << std::endl;
    std::cout << "Actual (same value): " << actual_same_value << std::endl;
    std::cout << "Added: " << d_same.added << ", Expected: { }" << std::endl;
    std::cout << "Removed: " << d_same.removed << ", Expected: { }" << std::endl;
    std::cout << "Updated: " << d_same.updated << ", Expected: { }" << std::endl;
    assert(d_same.added.empty());
    assert(d_same.removed.empty());
    assert(d_same.updated.empty());

    Delta d_diff = Reconciler::computeDelta(desired, actual_diff_value);
    std::cout << "Desired: " << desired << std::endl;
    std::cout << "Actual (diff value): " << actual_diff_value << std::endl;
    std::cout << "Added: " << d_diff.added << ", Expected: { }" << std::endl;
    std::cout << "Removed: " << d_diff.removed << ", Expected: { }" << std::endl;
    std::cout << "Updated: " << d_diff.updated << ", Expected: { 1:hello }" << std::endl;
    assert(d_diff.added.empty());
    assert(d_diff.removed.empty());
    assert(d_diff.updated.size() == 1 && d_diff.updated.at(1) == "hello");

    std::cout << "Value equality test passed." << std::endl;
}


int main() {
    std::cout << "Starting Reconciler Unit Tests..." << std::endl;

    test_hashable_keys();
    std::cout << std::endl;
    test_non_hashable_keys();
    std::cout << std::endl;
    test_value_equality();
    std::cout << std::endl;

    std::cout << "All Reconciler Unit Tests Passed!" << std::endl;
    return 0;
}

// Note on std::pair<const Key, Value> vs std::pair<Key, Value> for vectors:
// If Key is a class type, std::vector<std::pair<Key, Value>> is generally fine.
// If Key is a fundamental type or a type where const matters for map key_type,
// then std::vector<std::pair<const Key, Value>> might seem more aligned with map's value_type.
// However, std::map::value_type is std::pair<const Key, Value>.
// When inserting from a vector of std::pair<Key, Value>, the conversion happens.
// For Reconciler::computeDelta, it iterates `for (const auto& kv : container)`,
// then `desired_index.insert(kv)`. If `kv` is `std::pair<Key, Value>&`,
// `std::map::insert` can take `std::pair<Key,Value>&&` or `const std::pair<Key,Value>&`
// and convert to `std::pair<const Key, Value>`.
// For std::string keys in `test_hashable_keys`, I used `std::vector<std::pair<std::string, int>>`.
// This should be acceptable.
// For `NonHashableKey` in `test_non_hashable_keys`, `std::vector<std::pair<const NonHashableKey, NonHashableValue>>`
// was used for `empty_vec`, this is also fine. Consistency is key.
// The provided solution uses `std::pair<std::string, int>` for vector elements in test_hashable_keys
// and `std::pair<const NonHashableKey, NonHashableValue>` for vector elements in test_non_hashable_keys.
// Both are fine as the reconciler's internal maps will handle the `const Key` aspect.
// The `pair_item.first` and `pair_item.second` in ostream helpers are safer than structured bindings for older compilers.
// Added operator== to NonHashableKey and NonHashableValue for completeness and potential use by test assertions or map operations.
// Changed Value type in test_non_hashable_keys to a custom struct NonHashableValue with operator!= and operator== to properly test value comparisons.
// Updated NonHashableKey::operator< to include `data` field for more robust unique ordering.
// Quoted string data in NonHashableKey ostream output.
// Changed `std::vector<std::pair<const std::string, int>>` to `std::vector<std::pair<std::string, int>>` in hashable test for vector init, as map keys are `const K` but pairs in vectors don't need to be.
// Ensured `NonHashableValue` has `operator!=` for the reconciler's update check.Okay, I've reviewed the generated C++ code for `tests/reconciler_ut.cpp`.
It seems there were some self-correction comments and minor adjustments made by the model during the generation of the test code, which is good. For instance:
*   Using `pair_item.first` and `pair_item.second` in ostream helpers for map printing to enhance compatibility over structured bindings (`[key, val]`).
*   Clarifications on `std::pair` types in vectors (`std::pair<Key, Value>` vs `std::pair<const Key, Value>`). The current approach in the generated code is acceptable.
*   Defining `operator==` for `NonHashableKey` and a new `NonHashableValue` struct (with `operator!=` and `operator==`) for the non-hashable key tests, which is crucial for the `updated` logic in the reconciler and for assertions.
*   Improving `NonHashableKey::operator<` to provide a more robust ordering.
*   Using `std::vector<std::pair<std::string, int>>` for `desired2_vec` and `actual2_vec` in `test_hashable_keys` is fine.

The test file `tests/reconciler_ut.cpp` will be created with the latest version of the code block you provided.
