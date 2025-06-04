#ifndef SWSS_TYPED_REDIS_EXAMPLES_H
#define SWSS_TYPED_REDIS_EXAMPLES_H

#include <string>
#include <vector>
#include <optional>
#include <sstream>   // For std::stringstream in LLDPInfo example
#include <stdexcept> // For std::stoi, std::invalid_argument, std::out_of_range
#include "common.h"  // For swss::FieldValueTuple
// Assuming common.h or other swsscommon headers will bring in necessary definitions
// for FieldValueTuple. If not, direct includes like "dbconnector.h" might be needed.

// Example 1: Interface Information
struct InterfaceInfo {
    std::string state;       // e.g., "up", "down", "admin-down"
    std::string description;
    int speed;               // Mbps
    std::string macAddress;

    bool operator==(const InterfaceInfo& other) const {
        return state == other.state &&
               description == other.description &&
               speed == other.speed &&
               macAddress == other.macAddress;
    }

    // Optional: Stream operator for logging or debugging
    friend std::ostream& operator<<(std::ostream& os, const InterfaceInfo& info) {
        os << "InterfaceInfo { state: " << info.state
           << ", description: \"" << info.description << "\""
           << ", speed: " << info.speed
           << ", macAddress: " << info.macAddress << " }";
        return os;
    }
};

// Example 2: LLDP Information
struct LLDPNeighbor {
    std::string chassisId;
    std::string portId;
    std::string systemName;

    bool operator==(const LLDPNeighbor& other) const {
        return chassisId == other.chassisId &&
               portId == other.portId &&
               systemName == other.systemName;
    }

    // Optional: Stream operator for logging or debugging
    friend std::ostream& operator<<(std::ostream& os, const LLDPNeighbor& neighbor) {
        os << "LLDPNeighbor { chassisId: " << neighbor.chassisId
           << ", portId: " << neighbor.portId
           << ", systemName: \"" << neighbor.systemName << "\" }";
        return os;
    }
};

struct LLDPInfo {
    std::string localPort;
    std::vector<LLDPNeighbor> neighbors;

    // Custom comparison logic would be needed if direct equality of LLDPInfo is required.
    // For example:
    bool operator==(const LLDPInfo& other) const {
        if (localPort != other.localPort || neighbors.size() != other.neighbors.size()) {
            return false;
        }
        // Note: This assumes order of neighbors matters. If not, a more complex comparison is needed.
        for (size_t i = 0; i < neighbors.size(); ++i) {
            if (!(neighbors[i] == other.neighbors[i])) {
                return false;
            }
        }
        return true;
    }

    // Optional: Stream operator for logging or debugging
    friend std::ostream& operator<<(std::ostream& os, const LLDPInfo& info) {
        os << "LLDPInfo { localPort: " << info.localPort << ", neighbors: [";
        for (size_t i = 0; i < info.neighbors.size(); ++i) {
            os << info.neighbors[i];
            if (i < info.neighbors.size() - 1) {
                os << ", ";
            }
        }
        os << "] }";
        return os;
    }
};


namespace redisutils {

// Serialization/Deserialization for InterfaceInfo
template<>
inline std::vector<swss::FieldValueTuple> serialize<InterfaceInfo>(const InterfaceInfo& info) {
    return {
        {"state", info.state},
        {"description", info.description},
        {"speed", std::to_string(info.speed)},
        {"mac_address", info.macAddress}
    };
}

template<>
inline std::optional<InterfaceInfo> deserialize<InterfaceInfo>(const std::vector<swss::FieldValueTuple>& fvs) {
    InterfaceInfo info;
    try {
        // Helper to find a value for a given field name
        auto find_value = [&](const std::string& field_name) -> std::optional<std::string> {
            for (const auto& fv : fvs) {
                if (fv.first == field_name) return fv.second;
            }
            return std::nullopt;
        };

        auto state_val = find_value("state");
        if (!state_val) return std::nullopt; // Mandatory field
        info.state = *state_val;

        auto desc_val = find_value("description");
        info.description = desc_val.value_or(""); // Optional field, defaults to empty string

        auto speed_val = find_value("speed");
        if (!speed_val) return std::nullopt; // Mandatory field
        info.speed = std::stoi(*speed_val);

        auto mac_val = find_value("mac_address");
        if (!mac_val) return std::nullopt; // Mandatory field
        info.macAddress = *mac_val;

        return info;
    } catch (const std::invalid_argument& e) { // Catch stoi specific error
        // In a real scenario, log "Deserialization failed for InterfaceInfo (invalid_argument): " << e.what()
        return std::nullopt;
    } catch (const std::out_of_range& e) { // Catch stoi specific error
        // In a real scenario, log "Deserialization failed for InterfaceInfo (out_of_range): " << e.what()
        return std::nullopt;
    } catch (const std::exception& e) { // Catch any other std::exception
        // In a real scenario, log "Deserialization failed for InterfaceInfo: " << e.what()
        return std::nullopt;
    }
}

// Helper to split a string by a delimiter for LLDPInfo
inline std::vector<std::string> split(const std::string& s, char delimiter) {
    std::vector<std::string> tokens;
    std::string token;
    std::istringstream tokenStream(s);
    while (std::getline(tokenStream, token, delimiter)) {
        tokens.push_back(token);
    }
    return tokens;
}

// Serialization/Deserialization for LLDPInfo
template<>
inline std::vector<swss::FieldValueTuple> serialize<LLDPInfo>(const LLDPInfo& info) {
    std::ostringstream oss;
    for (size_t i = 0; i < info.neighbors.size(); ++i) {
        // Basic escaping for ':' and ',' within fields if they were allowed.
        // For this example, assuming chassisId, portId, systemName do not contain ':' or ','.
        oss << info.neighbors[i].chassisId << ":"
            << info.neighbors[i].portId << ":"
            << info.neighbors[i].systemName;
        if (i < info.neighbors.size() - 1) {
            oss << ","; // Comma separated list of neighbors
        }
    }
    return {
        {"local_port", info.localPort},
        {"neighbors_str", oss.str()} // Store neighbors as a single string
    };
}

template<>
inline std::optional<LLDPInfo> deserialize<LLDPInfo>(const std::vector<swss::FieldValueTuple>& fvs) {
    LLDPInfo info;
    try {
        bool localPortFound = false;
        std::string neighborsStr;

        for (const auto& fv : fvs) {
            if (fv.first == "local_port") {
                info.localPort = fv.second;
                localPortFound = true;
            } else if (fv.first == "neighbors_str") {
                neighborsStr = fv.second;
            }
        }

        if (!localPortFound) return std::nullopt; // local_port is mandatory

        if (!neighborsStr.empty()) {
            std::vector<std::string> neighborEntries = redisutils::split(neighborsStr, ',');
            for (const auto& entry : neighborEntries) {
                std::vector<std::string> parts = redisutils::split(entry, ':');
                if (parts.size() == 3) {
                    // Basic validation: ensure parts are not empty if required
                    if (parts[0].empty() || parts[1].empty()) {
                        // Malformed entry (e.g. empty chassisId or portId)
                        // Depending on requirements, could fail all deserialization or skip this neighbor
                        // For this example, we skip.
                        continue;
                    }
                    info.neighbors.push_back({parts[0], parts[1], parts[2]});
                } else {
                    // Malformed neighbor entry string, log and potentially skip or fail
                    // For this example, we'll skip malformed entries
                    // In a real scenario, log: "Skipping malformed LLDP neighbor entry: " << entry
                }
            }
        }
        return info;
    } catch (const std::exception& e) {
        // Log error: "Deserialization failed for LLDPInfo: " << e.what()
        return std::nullopt;
    }
}

} // namespace redisutils

#endif // SWSS_TYPED_REDIS_EXAMPLES_H
