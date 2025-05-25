// Go specific typemaps and helper structs for swsscommon

%{
#include "redisreply.h"
#include "dbconnector.h" // For SonicDBKey, RedisInstInfo
#include "stream.h"      // For Stream class and its complex return types
#include <vector>
#include <string>
#include <map>

// Helper structs for Go typemaps (C++ side)
// These structs will be what the C++ helper functions return,
// and SWIG will map these to Go structs.

// For std::vector<std::pair<std::string, std::string>>
struct GoFieldValue {
    std::string field;
    std::string value;
};
typedef std::vector<GoFieldValue> GoFieldValueList;

// For StreamReadResultVec: std::vector<std::pair<std::string, std::vector<std::pair<std::string, std::string>>>>
struct GoStreamMessage {
    std::string id;
    std::string fields_str; // Concatenated "field1:value1,field2:value2"
    // Alternatively, could be GoFieldValueList fields;
};
typedef std::vector<GoStreamMessage> GoStreamMessageList;

struct GoStreamReadEntry {
    std::string stream_name;
    GoStreamMessageList messages;
};
typedef std::vector<GoStreamReadEntry> GoStreamReadResult;


// For XPENDING summary
struct GoXPendingSummary {
    long long pending_message_count;
    std::string min_message_id;
    std::string max_message_id;
    // For simplicity, consumer details are initially omitted here, matching C API.
    // std::vector<std::pair<std::string, long long>> consumer_stats;
};

// For XPENDING detailed entry
struct GoXPendingDetailedEntry {
    std::string message_id;
    std::string consumer_name;
    long long idle_time_ms;
    long long delivery_count;
};
typedef std::vector<GoXPendingDetailedEntry> GoXPendingDetailedResult;

// Helper functions to be implemented in a .cxx file
// These will convert from the C++ classes' return types to these Go-friendly structs.

GoStreamReadResult convert_stream_read_result(
    const std::shared_ptr<std::vector<std::pair<std::string, std::vector<std::pair<std::string, std::string>>>>>& cpp_result);

GoXPendingSummary convert_xpending_summary(swss::RedisReply* reply);
GoXPendingDetailedResult convert_xpending_detailed(swss::RedisReply* reply);

// For DBConnector::hgetall
typedef std::map<std::string, std::string> GoStringMap;
// For Table::get (std::vector<std::pair<std::string, std::string>>&)
// GoFieldValueList is already defined

%}

// SWIG typemaps to convert C++ structs above to Go structs
// These will be used by SWIG when it sees functions returning these C++ structs/types.

%typemap(gotype) GoFieldValue "swsscommon.FieldValue";
%typemap(out) GoFieldValue {
    $result = swsscommon.NewFieldValue();
    $result.SetField($1.field);
    $result.SetValue($1.value);
}
%typemap(gotype) GoFieldValueList "[]swsscommon.FieldValue";
%typemap(out) GoFieldValueList {
    $result = swsscommon.NewDirectorFieldValueList($1.size()); // Assuming a director class FieldValueList is made
    for (size_t i = 0; i < $1.size(); ++i) {
        // Create GoFieldValue struct using a helper or direct assignment if possible
        // This part needs careful handling of how Go slices of structs are populated from C++ vector of structs
        // For now, assume a helper function or direct member access in the generated Go wrapper.
        // Let's assume SWIG generates basic struct mapping, and we populate a slice in Go.
        // More robust: use SWIG to generate methods to add to slice.
        // Simplified: Rely on SWIG's default vector-to-slice for structs if members are public or accessors exist.
        // If GoFieldValue is mapped to a Go type `swsscommon.FieldValue`,
        // then GoFieldValueList (std::vector<GoFieldValue>) should map to `[]swsscommon.FieldValue`.
        // SWIG handles this conversion if the element type (GoFieldValue) is correctly mapped.
    }
}

%typemap(gotype) GoStreamMessage "swsscommon.StreamMessage";
%typemap(out) GoStreamMessage {
    $result = swsscommon.NewStreamMessage();
    $result.SetId($1.id);
    $result.SetFields_str($1.fields_str);
}
%typemap(gotype) GoStreamMessageList "[]swsscommon.StreamMessage";
// Similar to GoFieldValueList, rely on SWIG's default vector-to-slice for structs.

%typemap(gotype) GoStreamReadEntry "swsscommon.StreamReadEntry";
%typemap(out) GoStreamReadEntry {
    $result = swsscommon.NewStreamReadEntry();
    $result.SetStream_name($1.stream_name);
    // $result.SetMessages($1.messages); // This needs mapping for GoStreamMessageList
}
%typemap(gotype) GoStreamReadResult "[]swsscommon.StreamReadEntry";
// Similar to GoFieldValueList, rely on SWIG's default vector-to-slice for structs.


%typemap(gotype) GoXPendingSummary "swsscommon.XPendingSummary";
%typemap(out) GoXPendingSummary {
    $result = swsscommon.NewXPendingSummary();
    $result.SetPending_message_count($1.pending_message_count);
    $result.SetMin_message_id($1.min_message_id);
    $result.SetMax_message_id($1.max_message_id);
}

%typemap(gotype) GoXPendingDetailedEntry "swsscommon.XPendingDetailedEntry";
%typemap(out) GoXPendingDetailedEntry {
    $result = swsscommon.NewXPendingDetailedEntry();
    $result.SetMessage_id($1.message_id);
    $result.SetConsumer_name($1.consumer_name);
    $result.SetIdle_time_ms($1.idle_time_ms);
    $result.SetDelivery_count($1.delivery_count);
}
%typemap(gotype) GoXPendingDetailedResult "[]swsscommon.XPendingDetailedEntry";
// Similar to GoFieldValueList, rely on SWIG's default vector-to-slice for structs.


// Typemap for std::shared_ptr<std::string> to Go string
%typemap(gotype) std::shared_ptr<std::string> "string";
%typemap(out) std::shared_ptr<std::string> {
    if ($1) {
        $result = ($1->empty() ? "" : _gostring_($1->c_str(), $1->length()));
    } else {
        $result = ""; // Or handle nil appropriately if Go type can be nil
    }
}
// Typemap for const std::vector<std::string>& (input)
%typemap(in) const std::vector<std::string>& (std::vector<std::string> temp) {
    temp.clear();
    if ($input != NULL && reflect.ValueOf($input).Kind() == reflect.Slice) {
        int len = reflect.ValueOf($input).Len();
        for (int i = 0; i < len; i++) {
            // Assuming $input is []string
            // This requires Go specific SWIG code for slice iteration
            // For now, this is a placeholder for correct Go slice to std::vector<std::string> conversion
            // std::string val = ... get from $input[i] ...;
            // temp.push_back(val);
        }
    }
    $1 = &temp;
}

// Typemap for const std::vector<std::pair<std::string, std::string>>& (input for xadd)
// This will use GoFieldValueList as input type for easier mapping by SWIG
%typemap(in) const std::vector<std::pair<std::string, std::string>>& (std::vector<std::pair<std::string, std::string>> temp_fvp) {
    // $input is expected to be []swsscommon.FieldValue (GoFieldValueList gotype)
    // Iterate over the Go slice and populate temp_fvp
    // This needs proper Go slice handling code in SWIG, similar to above.
    // For now, placeholder.
    // Example:
    // if ($input != NULL ...) {
    //   for (int i=0; i<len; ++i) {
    //     GoFieldValue go_fv = ... get from $input[i] ...;
    //     temp_fvp.push_back(std::make_pair(go_fv.field, go_fv.value));
    //   }
    // }
    $1 = &temp_fvp;
}


// Special handling for std::shared_ptr<swss::RedisReply> for xpending
// We will change Stream::xpending to return a GoXPendingSummary or GoXPendingDetailedResult
// by calling helper functions. So, direct mapping of shared_ptr<RedisReply> might not be needed
// if we modify the %extend block for Stream.

// Basic mapping for std::vector<std::string> to Go []string
%typemap(gotype) std::vector<std::string> "[]string"
%typemap(out) std::vector<std::string> {
    $result = _gostring_array_($1.data(), $1.size());
}


// Ensure SonicDBKey and RedisInstInfo are mapped if used by DBConnector methods exposed to Go
%typemap(gotype) swss::SonicDBKey "swsscommon.SonicDBKey";
%typemap(gotype) swss::RedisInstInfo "swsscommon.RedisInstInfo";
// ... add out typemaps for these if they are returned by value and need specific field mapping ...
// For now, assume SWIG default struct mapping works if members are public or have getters/setters.

// GoStringMap for hgetall
%typemap(gotype) GoStringMap "map[string]string";
%typemap(out) GoStringMap {
    $result = make(map[string]string);
    for (auto const& [key, val] : $1) {
        // Add to Go map: result[key_go] = val_go;
        // This requires specific SWIG Go map manipulation.
    }
}

// This file is intended to be included in the main swsscommon.i for Go.
// The actual implementation of helper functions (convert_*) goes into a .cxx file.
// The Go struct definitions (FieldValue, StreamMessage etc.) will be generated by SWIG
// in the swsscommon.go file based on these C++ structs and typemaps.
// The director class FieldValueList mentioned in GoFieldValueList typemap suggests
// that SWIG would generate a Go wrapper for std::vector<GoFieldValue> that has methods
// like Add(), Get(), etc. This is standard for directors with vectors.

// Placeholder for input typemap for FieldValuePairs (vector<pair<string,string>>)
// This would be used if a Go []FieldValue is passed to a C++ function expecting
// const std::vector<std::pair<std::string, std::string>>&
// %typemap(in) const std::vector<std::pair<std::string, std::string>>& (std::vector<std::pair<std::string, std::string>> temp) {
//    // $input would be the Go slice. Iterate and populate 'temp'.
//    // This is complex and requires careful handling of Go types in C++.
//    // For now, we will assume that for Stream::xadd, we can define an %extend method
//    // that takes the Go-friendly GoFieldValueList directly.
// }

// Similar placeholder for input std::vector<std::string>
// %typemap(in) const std::vector<std::string>& (std::vector<std::string> temp) {
//    // $input would be Go []string. Iterate and populate 'temp'.
// }

// Fallback for shared_ptr if specific mapping is not provided.
// SWIG generally handles shared_ptr by creating a proxy class that owns the pointer.
// %feature("smartptr", noptr) std::shared_ptr; // May simplify some default wrapping.

// For SonicDBConfig methods returning std::vector<std::string> or std::map
// Ensure appropriate typemaps or rely on default SWIG conversions.

// Default std::map to Go map conversion:
// SWIG can often map std::map<std::string, std::string> to map[string]string by default.
// If not, specific typemaps are needed.
// %typemap(gotype) std::map<std::string, std::string> "map[string]string"
// %typemap(out) std::map<std::string, std::string> { /* ... conversion code ... */ }
// %typemap(in) const std::map<std::string, std::string>& { /* ... conversion code ... */ }

// Similar for std::vector<std::pair<std::string, std::string>> for Table::get
// %typemap(gotype) std::vector<std::pair<std::string, std::string>> "[]swsscommon.FieldValue"
// %typemap(out) std::vector<std::pair<std::string, std::string>> { /* ... conversion ... */ }


// The typemaps for "out" are for return values.
// For input parameters (e.g., `const std::vector<std::string>& ids`),
// an "in" typemap is needed to convert from Go slice to C++ vector.
// These are more complex as they involve accessing Go runtime types from C++.
// SWIG provides some mechanisms using `reflect` package in Go side wrappers.

// For example, for `const std::vector<std::string>&` input:
// %typemap(in) const std::vector<std::string>& (std::vector<std::string> temp) {
//     temp.clear();
//     // Code to iterate over Go slice $input and fill temp
//     // This often involves generated Go helper code and Cgo calls.
//     // For simple cases, SWIG might generate this automatically.
//     $1 = &temp;
// }
// And its corresponding Go type:
// %typemap(gotype) const std::vector<std::string>& "[]string"

// For const std::vector<std::pair<std::string, std::string>>& (input for xadd)
// This will map to a Go slice of a struct, e.g. []FieldValue
// %typemap(gotype) const std::vector<std::pair<std::string, std::string>>& "[]swsscommon.FieldValue"
// %typemap(in) const std::vector<std::pair<std::string, std::string>>& (std::vector<std::pair<std::string, std::string>> temp_fvp) {
//     // Iterate over the Go slice $input (which is []swsscommon.FieldValue)
//     // and populate temp_fvp.
//     $1 = &temp_fvp;
// }

// These input typemaps are crucial for making the Go API idiomatic.
// If not provided, Go users would have to work with SWIG-generated C++ vector proxies.
// The actual implementation of these input typemaps can be quite involved.
// For now, I'll focus on the output typemaps and the %extend block for Stream,
// which might simplify some input handling by defining Go-friendly C++ facade methods.

// It's also common to use %typemap(jstype) for consistency if JavaScript is also a target,
// but here we focus on 'gotype'.
Okay, I've created a preliminary `goext/go_typemaps.i` file. This file is designed to hold Go-specific SWIG typemaps and declare helper C++ structures and functions that will facilitate the conversion of C++ types to Go types.

**Key aspects of `goext/go_typemaps.i`:**

*   **C++ Helper Structs (`GoFieldValue`, `GoStreamMessage`, etc.)**: These are simplified C++ structs that mirror the desired Go struct layout. The idea is that C++ helper functions will convert the more complex `swss::Stream` return types into these simpler C++ structs. SWIG will then find it easier to map these simpler C++ structs to the target Go structs.
*   **Declarations of Helper Functions**: Functions like `convert_stream_read_result`, `convert_xpending_summary`, `convert_xpending_detailed` are declared. These will be implemented in a new `.cxx` file and are crucial for bridging the C++ and Go worlds for complex types.
*   **SWIG Typemaps (`%typemap(gotype)`, `%typemap(out)`)**: These instruct SWIG on how to map the C++ helper structs (and some standard types like `std::shared_ptr<std::string>`) to Go types and how to convert them when they are returned from C++ functions.
    *   The `out` typemaps often involve creating a new Go object (e.g., `swsscommon.NewStreamMessage()`) and setting its fields. This assumes that SWIG will generate Go structs with corresponding setter methods or public fields.
    *   For `std::vector` of these helper structs, the typemap relies on SWIG's default capability to convert a `std::vector<SomeMappedStruct>` into a Go slice `[]SomeMappedGoStruct`, provided `SomeMappedStruct` itself is correctly typemapped.
*   **Input Typemaps (Placeholders)**: Basic placeholders and comments for input typemaps (e.g., for `const std::vector<std::string>&`) are included. These are more complex to implement fully as they require SWIG to generate Cgo code that interacts with Go slices. For now, the strategy might involve using `%extend` to create Go-friendlier C++ methods if direct input typemapping is too hard.

Now, I will modify `goext/swsscommon.i`. This involves:
1.  Removing Python-specific directives.
2.  Including `goext/go_typemaps.i`.
3.  Adding `%include "stream.h"`.
4.  Using `%extend swss::Stream` to define wrapper methods that call the C++ helper functions (declared in `go_typemaps.i`) for complex return types. This makes the original C++ methods effectively hidden from Go, and Go calls these new extended methods.

Here's the modification of `goext/swsscommon.i`:
