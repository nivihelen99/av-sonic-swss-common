#include "pubsub_config_loader.h"
#include "logger.h"     // For SWSS_LOG_WARN, SWSS_LOG_INFO, SWSS_LOG_ERROR
#include "json.h"       // For potential future use if config values are complex JSON strings

namespace swss {

common::DeliveryMode deliveryModeFromString(const std::string& mode_str, common::DeliveryMode default_mode) {
    std::string lower_mode_str = mode_str;
    std::transform(lower_mode_str.begin(), lower_mode_str.end(), lower_mode_str.begin(),
                   [](unsigned char c){ return std::tolower(c); });

    if (lower_mode_str == "at-most-once") {
        return common::DeliveryMode::AT_MOST_ONCE;
    } else if (lower_mode_str == "at-least-once") {
        return common::DeliveryMode::AT_LEAST_ONCE;
    } else if (lower_mode_str == "exactly-once") {
        return common::DeliveryMode::EXACTLY_ONCE;
    } else {
        SWSS_LOG_WARN("Unrecognized delivery mode string: '%s'. Defaulting to %d.",
                      mode_str.c_str(), static_cast<int>(default_mode));
        return default_mode;
    }
}

// Implementation for PubSubConfig::parseDeliveryMode (if it wasn't in the header)
// void PubSubConfig::parseDeliveryMode() {
//     delivery_mode_enum = deliveryModeFromString(delivery_mode_str, common::DeliveryMode::AT_LEAST_ONCE);
// }


bool loadPubSubConfig(swss::ConfigDBConnector* configDb,
                      const std::string& channelName,
                      swss::PubSubConfig& config_out) {
    if (!configDb || !configDb->isDbConnected(std::string(CONFIG_DB_NAME))) {
        SWSS_LOG_ERROR("ConfigDBConnector is null or not connected. Cannot load PubSub config for channel '%s'.",
                       channelName.c_str());
        return false;
    }

    std::string config_key_in_db = channelName.empty() || channelName == CFG_ADVANCED_PUBSUB_GLOBAL_KEY ?
                                   CFG_ADVANCED_PUBSUB_GLOBAL_KEY : channelName;

    std::string full_config_table_entry = CFG_ADVANCED_PUBSUB_TABLE_NAME + configDb->getTableNameSeparator() + config_key_in_db;

    std::map<std::string, std::string> config_values;
    configDb->hgetall(full_config_table_entry, config_values);

    if (config_values.empty()) {
        SWSS_LOG_INFO("No specific PubSub config found for entry '%s'. Using defaults or previously loaded global config.",
                      full_config_table_entry.c_str());
        // Call parseDeliveryMode to ensure enum is consistent with default string,
        // or if called after global, it ensures string from global is parsed.
        config_out.parseDeliveryMode();
        return false; // No specific config for this key was loaded, but config_out might have defaults or global values.
    }

    SWSS_LOG_INFO("Loading PubSub config for entry '%s'. Found %zu fields.",
                  full_config_table_entry.c_str(), config_values.size());
    bool loaded_any_value = false;

    try {
        if (config_values.count("default_priority")) {
            config_out.default_priority = std::stoi(config_values.at("default_priority"));
            loaded_any_value = true;
        }
        if (config_values.count("max_queue_depth")) {
            config_out.max_queue_depth = std::stoi(config_values.at("max_queue_depth"));
            loaded_any_value = true;
        }
        if (config_values.count("ack_timeout_ms")) {
            config_out.ack_timeout_ms = std::stoi(config_values.at("ack_timeout_ms"));
            loaded_any_value = true;
        }
        if (config_values.count("max_retries")) {
            config_out.max_retries = std::stoi(config_values.at("max_retries"));
            loaded_any_value = true;
        }
        if (config_values.count("delivery_mode_str")) { // Load the string representation
            config_out.delivery_mode_str = config_values.at("delivery_mode_str");
            // The enum is parsed by calling parseDeliveryMode() below
            loaded_any_value = true;
        }
        if (config_values.count("dead_letter_retention_hours")) {
            config_out.dead_letter_retention_hours = std::stoi(config_values.at("dead_letter_retention_hours"));
            loaded_any_value = true;
        }
        if (config_values.count("enable_message_persistence")) {
            std::string val_str = config_values.at("enable_message_persistence");
            std::transform(val_str.begin(), val_str.end(), val_str.begin(), ::tolower);
            config_out.enable_message_persistence = (val_str == "true" || val_str == "1");
            loaded_any_value = true;
        }
        if (config_values.count("filter_cache_size")) {
            config_out.filter_cache_size = std::stoi(config_values.at("filter_cache_size"));
            loaded_any_value = true;
        }
        // Add parsing for other fields from PubSubConfig as they are defined in JSON schema
    } catch (const std::invalid_argument& ia) {
        SWSS_LOG_ERROR("PubSubConfig: Invalid argument parsing field for '%s': %s", full_config_table_entry.c_str(), ia.what());
        // Depending on strictness, might return false or continue with defaults for failed fields.
    } catch (const std::out_of_range& oor) {
        SWSS_LOG_ERROR("PubSubConfig: Out of range parsing field for '%s': %s", full_config_table_entry.c_str(), oor.what());
    }

    // Ensure the enum value is updated based on the string value (either default or loaded)
    config_out.parseDeliveryMode();

    if (loaded_any_value) {
         SWSS_LOG_INFO("Successfully loaded PubSub config for entry '%s'. Delivery mode string: '%s', enum: %d",
                       full_config_table_entry.c_str(), config_out.delivery_mode_str.c_str(), static_cast<int>(config_out.delivery_mode_enum));
    }
    return loaded_any_value;
}

} // namespace swss
