*static analysis:*

[![Total alerts](https://img.shields.io/lgtm/alerts/g/sonic-net/sonic-swss-common.svg?logo=lgtm&logoWidth=18)](https://lgtm.com/projects/g/sonic-net/sonic-swss-common/alerts/)
[![Language grade: Python](https://img.shields.io/lgtm/grade/python/g/sonic-net/sonic-swss-common.svg?logo=lgtm&logoWidth=18)](https://lgtm.com/projects/g/sonic-net/sonic-swss-common/context:python)
[![Language grade: C/C++](https://img.shields.io/lgtm/grade/cpp/g/sonic-net/sonic-swss-common.svg?logo=lgtm&logoWidth=18)](https://lgtm.com/projects/g/sonic-net/sonic-swss-common/context:cpp)

*sonic-swss-common builds:*

[![master build](https://dev.azure.com/mssonic/build/_apis/build/status/Azure.sonic-swss-common?branchName=master&label=master)](https://dev.azure.com/mssonic/build/_build/latest?definitionId=9&branchName=master)
[![202205 build](https://dev.azure.com/mssonic/build/_apis/build/status/Azure.sonic-swss-common?branchName=202205&label=202205)](https://dev.azure.com/mssonic/build/_build/latest?definitionId=9&branchName=202205)
[![202111 build](https://dev.azure.com/mssonic/build/_apis/build/status/Azure.sonic-swss-common?branchName=202111&label=202111)](https://dev.azure.com/mssonic/build/_build/latest?definitionId=9&branchName=202111)
[![202106 build](https://dev.azure.com/mssonic/build/_apis/build/status/Azure.sonic-swss-common?branchName=202106&label=202106)](https://dev.azure.com/mssonic/build/_build/latest?definitionId=9&branchName=202106)
[![202012 build](https://dev.azure.com/mssonic/build/_apis/build/status/Azure.sonic-swss-common?branchName=202012&label=202012)](https://dev.azure.com/mssonic/build/_build/latest?definitionId=9&branchName=202012)
[![201911 build](https://dev.azure.com/mssonic/build/_apis/build/status/Azure.sonic-swss-common?branchName=201911&label=201911)](https://dev.azure.com/mssonic/build/_build/latest?definitionId=9&branchName=201911)

# SONiC - SWitch State Service Common Library - SWSS-COMMON

## Description
The SWitch State Service (SWSS) common library provides libraries for database communications, netlink wrappers, and other functions needed by SWSS.

## Getting Started

### Build from Source

Checkout the source:

    git clone --recursive https://github.com/sonic-net/sonic-swss-common


Install build dependencies:

    sudo apt-get install make libtool m4 autoconf dh-exec debhelper cmake pkg-config \
                         libhiredis-dev libnl-3-dev libnl-genl-3-dev libnl-route-3-dev \
                         libnl-nf-3-dev swig3.0 libpython2.7-dev libpython3-dev \
                         libgtest-dev libgmock-dev libboost-dev

Build and Install Google Test and Mock from DEB source packages:

    cd /usr/src/gtest && sudo cmake . && sudo make

You can compile and install from source using:

    ./autogen.sh
    ./configure
    make && sudo make install

You can also build a debian package using:

    ./autogen.sh
    ./configure
    dpkg-buildpackage -us -uc -b

### Build with Google Test
1. Rebuild with Google Test
```
$ ./autogen.sh
$ ./configure --enable-debug 'CXXFLAGS=-O0 -g'
$ make clean
$ GCC_COLORS=1 make
```

2. Start redis server if not yet:
```
sudo sed -i 's/notify-keyspace-events ""/notify-keyspace-events AKE/' /etc/redis/redis.conf
sudo service redis-server start
```

3. Run unit test:
```
tests/tests
```

## TypedTable: Strongly-Typed Redis Interface

`TypedTable<T>` is a C++ template class that provides a type-safe wrapper around the existing `swss::Table` for interacting with Redis. It aims to reduce boilerplate code for data serialization and deserialization and enables an event-driven programming model for table updates.

The definition can be found in `common/typed_redis.h`. For usage examples of different data structures, see `common/typed_redis_examples.h`.

### Features

*   **Strongly-Typed Operations**: Enforces type safety at compile time for `set(key, T)` and `get(key) -> std::optional<T>` operations.
*   **Automatic Serialization/Deserialization**: Converts C++ objects to Redis hash fields and vice-versa.
*   **Event-Driven Updates**: Allows subscription to table changes via the `onChange(callback)` method, where callbacks receive the key and the deserialized object.
*   **User-Defined Types**: Can be used with any C++ struct or class `T` for which serialization/deserialization functions are provided.

### Defining Your Data Type and Serialization

To use `TypedTable<T>`, you first need to define your C++ struct or class `T`. Then, you must provide template specializations for `serialize<T>` and `deserialize<T>` functions within the `redisutils` namespace. These functions will handle the conversion between your type `T` and `std::vector<swss::FieldValueTuple>`, which is the format used by the underlying `swss::Table`.

```cpp
#include <string>
#include <vector>
#include <optional>
#include "common.h" // For swss::FieldValueTuple, or dbconnector.h

// Define your custom data structure
struct MyData {
    std::string name;
    int value;
    bool active;
    // Add operator== for testing or comparison if needed
    bool operator==(const MyData& other) const {
        return name == other.name && value == other.value && active == other.active;
    }
};

namespace redisutils {

// Implement serialization for MyData
// This function converts a MyData object into a vector of FieldValueTuple for Redis.
template<>
inline std::vector<swss::FieldValueTuple> serialize<MyData>(const MyData& data) {
    return {
        {"name", data.name},
        {"value", std::to_string(data.value)}, // Convert numbers to strings
        {"active", data.active ? "true" : "false"} // Convert booleans to "true" or "false"
    };
}

// Implement deserialization for MyData
// This function converts a vector of FieldValueTuple from Redis into a std::optional<MyData>.
template<>
inline std::optional<MyData> deserialize<MyData>(const std::vector<swss::FieldValueTuple>& fvs) {
    MyData data;
    bool name_found = false;
    bool value_found = false;
    bool active_found = false;

    try {
        if (fvs.empty()) return std::nullopt; // No data to parse

        for (const auto& fv : fvs) {
            if (fv.first == "name") {
                data.name = fv.second;
                name_found = true;
            } else if (fv.first == "value") {
                data.value = std::stoi(fv.second); // Convert string back to int
                value_found = true;
            } else if (fv.first == "active") {
                data.active = (fv.second == "true"); // Convert string back to bool
                active_found = true;
            }
        }
        // Example: Check if all mandatory fields were found. Adjust as per your type's requirements.
        if (!name_found || !value_found || !active_found) {
            // Optionally log: "MyData deserialization: missing mandatory fields."
            return std::nullopt;
        }
        return data;
    } catch (const std::invalid_argument& e) {
        // std::cerr << "Deserialization failed for MyData (invalid_argument): " << e.what() << std::endl;
        return std::nullopt;
    } catch (const std::out_of_range& e) {
        // std::cerr << "Deserialization failed for MyData (out_of_range): " << e.what() << std::endl;
        return std::nullopt;
    } catch (...) {
        // std::cerr << "Unknown deserialization error for MyData." << std::endl;
        return std::nullopt;
    }
}

} // namespace redisutils
```
**Note:** Ensure your `serialize` and `deserialize` functions are robust. For `deserialize`, handle potential exceptions (e.g., from `std::stoi`) and missing fields gracefully by returning `std::nullopt`. Refer to `common/typed_redis_examples.h` for more examples like `InterfaceInfo` and `LLDPInfo`.

### Using TypedTable

Once your type `T` and its `redisutils` serialization functions are defined, you can use `TypedTable<T>`:

```cpp
#include "common/typed_redis.h"
// Assuming MyData struct and redisutils functions from above are defined and included
#include <iostream>
#include <thread> // For std::this_thread::sleep_for
#include <chrono> // For std::chrono::seconds

// (MyData struct and redisutils namespace specializations should be here or included)

int main() {
    // Connect to APPL_DB (default), for table "MY_DATA_TABLE"
    swss::TypedTable<MyData> myDataTable("MY_DATA_TABLE");

    // Example: Setting data
    MyData data_to_set = {"item_A", 123, true};
    myDataTable.set("key1", data_to_set);
    std::cout << "Set data for key1" << std::endl;

    // Example: Getting data
    std::optional<MyData> retrieved_data = myDataTable.get("key1");
    if (retrieved_data) {
        std::cout << "Retrieved for key1: Name=" << retrieved_data->name
                  << ", Value=" << retrieved_data->value
                  << ", Active=" << (retrieved_data->active ? "yes" : "no") << std::endl;
    } else {
        std::cout << "Data for key1 not found or is corrupted." << std::endl;
    }

    std::optional<MyData> non_existent_data = myDataTable.get("key_non_existent");
    if (!non_existent_data) {
        std::cout << "Correctly failed to retrieve non_existent_key." << std::endl;
    }

    // Example: Subscribing to changes
    // The callback will be invoked in a separate thread for each relevant change.
    myDataTable.onChange([](const std::string& key, const MyData& data) {
        std::cout << "Change detected for key '" << key << "': Name=" << data.name
                  << ", Value=" << data.value
                  << ", Active=" << (data.active ? "yes" : "no") << std::endl;
    });
    std::cout << "Subscribed to changes on MY_DATA_TABLE." << std::endl;

    // Simulate an update from another process/thread for demonstration
    // In a real scenario, another component would write to this Redis table.
    std::thread publisher_thread([&]() {
        std::this_thread::sleep_for(std::chrono::seconds(1));
        MyData updated_data = {"item_A_updated", 456, false};
        // Use a raw swss::Table to simulate an external update
        swss::DBConnector db("APPL_DB", 0);
        swss::Table raw_table(&db, "MY_DATA_TABLE");
        raw_table.set("key1", redisutils::serialize(updated_data));
        std::cout << "[Publisher] Updated key1" << std::endl;

        std::this_thread::sleep_for(std::chrono::seconds(1));
        MyData new_data = {"item_B", 789, true};
        raw_table.set("key2", redisutils::serialize(new_data));
        std::cout << "[Publisher] Set key2" << std::endl;
    });

    // Keep the main thread alive to allow onChange to process events.
    // In a real application, this would be part of a larger event loop or service.
    std::cout << "Main thread sleeping for a few seconds to observe changes..." << std::endl;
    std::this_thread::sleep_for(std::chrono::seconds(5));

    if (publisher_thread.joinable()) {
        publisher_thread.join();
    }

    std::cout << "Exiting example." << std::endl;
    // TypedTable destructor will automatically handle joining its internal thread.
    return 0;
}
```

### `onChange` Threading Model

The `onChange` method starts a background thread dedicated to listening for key-space notifications from Redis. When a relevant notification is received, this thread retrieves the data, deserializes it, and then invokes the user-provided callback with the key and the deserialized object `T`.

The destructor of `TypedTable<T>` (`~TypedTable()`) will automatically attempt to stop and join this background thread. Ensure that the `TypedTable` object remains in scope for as long as you need to receive notifications.

### Error Handling

*   **`get(key)`**: If the key is not found in Redis, or if the data stored at the key cannot be successfully deserialized into type `T` (e.g., due to malformed data or a `std::exception` during `redisutils::deserialize<T>`), `get()` will return `std::nullopt`.
*   **`onChange(callback)`**: If data associated with a changed key cannot be deserialized by `redisutils::deserialize<T>` (i.e., it returns `std::nullopt` or throws an exception that `TypedTable` internally catches), the callback will not be invoked for that specific malformed entry. This prevents crashes in the notification thread due to bad data. It is crucial that your `redisutils::deserialize<T>` implementation is robust and returns `std::nullopt` for any data it cannot correctly parse.

## Need Help?

For general questions, setup help, or troubleshooting:
- [sonicproject on Google Groups](https://groups.google.com/g/sonicproject)

For bug reports or feature requests, please open an Issue.

## Contribution guide

Please read the [contributors guide](https://github.com/sonic-net/SONiC/blob/gh-pages/CONTRIBUTING.md) for information about how to contribute.

All contributors must sign an [Individual Contributor License Agreement (ICLA)](https://docs.linuxfoundation.org/lfx/easycla/v2-current/contributors/individual-contributor) before contributions can be accepted. This process is managed by the [Linux Foundation - EasyCLA](https://easycla.lfx.linuxfoundation.org/) and automated
via a GitHub bot. If the contributor has not yet signed a CLA, the bot will create a comment on the pull request containing a link to electronically sign the CLA.

### GitHub Workflow

We're following basic GitHub Flow. If you have no idea what we're talking about, check out [GitHub's official guide](https://guides.github.com/introduction/flow/). Note that merge is only performed by the repository maintainer.

Guide for performing commits:

* Isolate each commit to one component/bugfix/issue/feature
* Use a standard commit message format:

>     [component/folder touched]: Description intent of your changes
>
>     [List of changes]
>
> 	  Signed-off-by: Your Name your@email.com

For example:

>     swss-common: Stabilize the ConsumerTable
>
>     * Fixing autoreconf
>     * Fixing unit-tests by adding checkers and initialize the DB before start
>     * Adding the ability to select from multiple channels
>     * Health-Monitor - The idea of the patch is that if something went wrong with the notification channel,
>       we will have the option to know about it (Query the LLEN table length).
>
>       Signed-off-by: user@dev.null


* Each developer should fork this repository and [add the team as a Contributor](https://help.github.com/articles/adding-collaborators-to-a-personal-repository)
* Push your changes to your private fork and do "pull-request" to this repository
* Use a pull request to do code review
* Use issues to keep track of what is going on
