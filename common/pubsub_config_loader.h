#pragma once
#include "advanced_pubsub_types.h" // For swss::PubSubConfig and common::DeliveryMode
#include "dbconnector.h"     // For swss::DBConnector (used by ConfigDBConnector)
#include "configdb.h"        // For swss::ConfigDBConnector
#include <string>
#include <map>      // For std::map
#include <vector>   // For std::vector (used by ConfigDBConnector hgetall)
#include <algorithm> // For std::transform
#include <cctype>    // For std::tolower

namespace swss {

// Constant for the configuration table name in CONFIG_DB
const std::string CFG_ADVANCED_PUBSUB_TABLE_NAME = "ADVANCED_PUBSUB_CONFIG";
const std::string CFG_ADVANCED_PUBSUB_GLOBAL_KEY = "global";


// Helper to convert string to DeliveryMode
// Note: This is also available as a member function PubSubConfig::parseDeliveryMode.
// This free function can be used independently if needed.
common::DeliveryMode deliveryModeFromString(const std::string& mode_str, common::DeliveryMode default_mode = common::DeliveryMode::AT_LEAST_ONCE);

// Attempts to load PubSub configuration for a given channel/table name.
// If tableName is empty or "global", attempts to load global configuration.
// The configuration is loaded from CONFIG_DB from a key like "ADVANCED_PUBSUB_CONFIG|tableName".
// If a specific table config is not found, it can optionally fall back to global (not implemented here, caller can do that).
// Returns true if a configuration was found and at least one value was loaded, false otherwise.
// config_out will be updated with loaded values; fields not present in DB remain as their defaults in config_out.
bool loadPubSubConfig(swss::ConfigDBConnector* configDb,
                      const std::string& channelName, // Specific channel/table, or empty/ "global" for global
                      swss::PubSubConfig& config_out);

} // namespace swss
