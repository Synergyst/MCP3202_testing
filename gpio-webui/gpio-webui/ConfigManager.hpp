#pragma once
#include <string>
#include <map>
#include <memory>
#include "PinConfig.hpp"

class ConfigManager {
public:
    ConfigManager(const std::string& path);
    bool load(std::map<int, std::shared_ptr<PinState>>& registry, int& timeout_ms);
    void save(const std::map<int, std::shared_ptr<PinState>>& registry, int timeout_ms);

    // Generic settings management
    std::string getSetting(const std::string& key, const std::string& defaultValue = "");
    void setSetting(const std::string& key, const std::string& value);

private:
    std::string config_path;
};
