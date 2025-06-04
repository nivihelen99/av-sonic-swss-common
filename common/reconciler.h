#pragma once

#include <algorithm>
#include <map>
#include <set>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <vector>

namespace aos_utils
{

namespace detail
{
// Type trait to check if a type is hashable
template <typename T, typename = void>
struct is_hashable : std::false_type
{
};

template <typename T>
struct is_hashable<T, std::void_t<decltype(std::declval<std::hash<T>>()(std::declval<T>()))>>
    : std::true_type
{
};

// Alias for hashability check
template <typename T>
inline constexpr bool is_hashable_v = is_hashable<T>::value;
} // namespace detail

template <typename Key, typename Value>
class Reconciler
{
public:
   // Defines the structure to hold the computed differences.
   struct Delta
   {
      std::set<Key> added; // Keys present in 'desired' but not in 'actual'.
      std::set<Key> removed; // Keys present in 'actual' but not in 'desired'.

      // Changed: Use a map to store updated keys along with their new desired values.
      // The type of map depends on whether Key is hashable.
      using UpdatedMapType = std::conditional_t<detail::is_hashable_v<Key>,
                                                std::unordered_map<Key, Value>,
                                                std::map<Key, Value>>;
      UpdatedMapType updated; // Keys present in both, but with different values.
         // Stores the key and its 'desired' (new) value.
   };

   /**
     * @brief Computes the delta between a desired state and an actual state.
     *
     * This function compares two containers (e.g., std::map, std::unordered_map,
     * or any container of key-value pairs that can be converted to a map)
     * and identifies elements that have been added, removed, or updated.
     *
     * @tparam DesiredContainer The type of the container holding the desired state.
     * @tparam ActualContainer The type of the container holding the actual state.
     * @param desired The container representing the desired state.
     * @param actual The container representing the actual state.
     * @return A Delta struct containing sets of added, removed, and updated keys (with their new values).
     */
   template <typename DesiredContainer, typename ActualContainer>
   static Delta computeDelta(const DesiredContainer& desired, const ActualContainer& actual)
   {
      Delta delta;

      using MapType = std::conditional_t<detail::is_hashable_v<Key>,
                                         std::unordered_map<Key, Value>,
                                         std::map<Key, Value>>;

      MapType desired_map_storage; // Temporary storage if desired is not MapType
      const MapType* desired_map_ptr = nullptr;

      if constexpr (std::is_same_v<std::decay_t<DesiredContainer>, MapType>) {
          desired_map_ptr = &desired; // Use desired directly
      } else {
          for (const auto& kv : desired) { // Populate storage from desired
              desired_map_storage.insert(kv);
          }
          desired_map_ptr = &desired_map_storage;
      }
      const MapType& desired_index = *desired_map_ptr;

      MapType actual_map_storage; // Temporary storage if actual is not MapType
      const MapType* actual_map_ptr = nullptr;

      if constexpr (std::is_same_v<std::decay_t<ActualContainer>, MapType>) {
          actual_map_ptr = &actual; // Use actual directly
      } else {
          // Populate storage from actual
          // Ensure we handle generic kv pairs correctly, assuming .first and .second
          // or that kv is directly insertable into MapType.
          // The original code used `actual_index.insert(kv)` which implies kv is suitable.
          for (const auto& kv : actual) {
              actual_map_storage.insert(kv);
          }
          actual_map_ptr = &actual_map_storage;
      }
      const MapType& actual_index = *actual_map_ptr;

      // --- Determine Added and Updated items ---
      // Iterate through the desired state to find additions and updates.
      for (const auto& pair_ref : desired_index)
      {
         const auto& desired_key = pair_ref.first;
         const auto& desired_value = pair_ref.second;
         auto it_actual = actual_index.find(desired_key);
         if (it_actual == actual_index.end())
         {
            // Key in desired but not in actual -> Added
            delta.added.insert(desired_key);
         }
         else
         {
            // Key in both, check if value is different -> Updated
            if (it_actual->second != desired_value)
            {
               delta.updated.insert({desired_key, desired_value}); // Store the new desired value
            }
         }
      }

      // --- Determine Removed items ---
      // Iterate through the actual state to find removals.
      for (const auto& pair_ref : actual_index)
      {
         const auto& actual_key = pair_ref.first;
         auto it_desired = desired_index.find(actual_key);
         if (it_desired == desired_index.end())
         {
            // Key in actual but not in desired -> Removed
            delta.removed.insert(actual_key);
         }
      }
      return delta;
   }
};

} // namespace aos_utils
