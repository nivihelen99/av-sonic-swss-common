
# 📝 Requirement.md

**Title**: Typed Redis Interface and Event Subscription API for SWSS Common

---

## 📌 Summary

This document outlines the requirements for extending `swsscommon` with:

1. **Strongly-Typed Redis Interface**
2. **Event-Based Subscription API**
3. **Unit Test Scaffolding and Mock Support**

The goal is to modernize and simplify Redis access, reduce boilerplate, improve type safety, and enable reactive programming patterns for SONiC application developers.

---

## 🎯 Goals

* Enable Redis interaction with strongly-typed C++ structures (`TypedTable<T>`)
* Provide event-based subscription hooks for table/key updates (`onChange`)
* Eliminate repeated serialization/parsing boilerplate
* Improve code maintainability and IDE tooling (autocompletion, compile-time checks)
* Provide a clean testing interface with mocks for simulation in unit tests

---

## 🧩 Functional Requirements

### 1. TypedTable Interface

* A `TypedTable<T>` class shall be introduced to wrap access to a Redis table.
* Users can:

  * `set(key, T)` – Store a typed object into Redis.
  * `get(key)` – Retrieve and deserialize a typed object from Redis.
* The `T` type must be user-defined and support conversion to/from `FieldValueTuple[]`.

#### Example:

```cpp
TypedTable<PortInfo> portTable("PORT_TABLE");
PortInfo data = { .adminStatus = "up", .mtu = 9000, .operUp = true };
portTable.set("Ethernet0", data);
```

---

### 2. Serialization Layer

* Users must define:

  * `std::vector<FieldValueTuple> serialize(const T&)`
  * `std::optional<T> deserialize(const std::vector<FieldValueTuple>&)`
* The library will invoke these functions for Redis operations.

#### Example:

```cpp
std::vector<FieldValueTuple> serialize(const PortInfo&);
std::optional<PortInfo> deserialize(const std::vector<FieldValueTuple>&);
```

---

### 3. Event Subscription API

* `TypedTable<T>::onChange(callback)` will allow clients to subscribe to changes in Redis.
* The callback should receive:

  * Redis key
  * Deserialized `T` struct
* Must support multiple events using `ConsumerTable` and `Select`.

#### Example:

```cpp
portTable.onChange([](const std::string& key, const PortInfo& info) {
    std::cout << "Port updated: " << key << ", status=" << info.adminStatus << "\n";
});
```

---

### 4. Threading

* The event loop must run in a background thread internally.
* Must be safely destructible (i.e., auto join on destructor).
* Thread-safe access to Redis (already supported by existing `DBConnector` and `Table`).

---

### 5. Error Handling

* `get()` should return `std::nullopt` on:

  * Key not found
  * Deserialization error
* Event callbacks should not crash the thread on deserialization failure.
* Optional `onError` handler for reporting malformed records (future extension).

---

### 6. Unit Test Support

* Provide mockable base interface for `TypedTable<T>`, allowing:

  * Overriding `set()` and `get()` for test doubles.
  * Injection of test data for `onChange()` simulation.

#### Example:

```cpp
MockTypedTable<PortInfo> mock;
mock.set("Ethernet42", {.adminStatus = "down", .mtu = 1500, .operUp = false});
```

---

### 7. Backward Compatibility

* No changes to existing `Table`, `ConsumerTable`, or `DBConnector` APIs.
* Typed interface builds on top of them.

---

## 🛠️ Non-Functional Requirements

| Requirement   | Value                                  |
| ------------- | -------------------------------------- |
| Language      | C++17                                  |
| Memory Safety | RAII-based, thread-safe components     |
| Redis Version | Compatible with Redis v5/v6            |
| Integration   | SONiC SWSS `libswsscommon`             |
| Documentation | Required for public API surface        |
| Testing       | GTest-based unit tests for all modules |

---

## 📂 Deliverables

* `typed_redis.hpp` and `typed_redis.cpp`
* Examples for:

  * PortInfo
  * Interface state
  * LLDP info
* Unit test: `test_typed_redis.cpp`
* README update for usage
* Optional: Python/Pybind extension (TBD)

---

## ✅ Acceptance Criteria

* Typed struct can be saved and retrieved from Redis using `set()` and `get()`
* Changes to Redis trigger `onChange()` callback with proper data
* Unit tests must:

  * Validate get/set round-trip
  * Simulate `onChange()` event stream
  * Cover error handling paths
* Works with existing SONiC Redis DB schema

---

## 🧭 Future Extensions (Optional)

* `onAdd()`, `onDelete()`, `onUpdate()` fine-grained callbacks
* JSON/BSON-based encoding for nested structures
* Python binding for `TypedTable` APIs
* Schema-driven codegen for `T` struct + serialization


