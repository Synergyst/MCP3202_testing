#include "ConfigManager.hpp"
#include <nlohmann/json.hpp>
#include <fstream>
#include <iostream>

using json = nlohmann::json;

ConfigManager::ConfigManager(const std::string& path) : config_path(path) {}

bool ConfigManager::load(std::map<int, std::shared_ptr<PinState>>& registry, int& timeout_ms) {
    std::ifstream f(config_path);
    if (!f.is_open()) {
        std::cout << "[CONFIG] No config file found. Using defaults." << std::endl;
        return false;
    }

    try {
        json j;
        f >> j;
        timeout_ms = j.value("timeout_ms", 5000);
        
        if (j.contains("pins")) {
            for (auto& [key, value] : j["pins"].items()) {
                int phys = std::stoi(key);
                if (registry.count(phys)) {
                    auto pin = registry[phys];
                    std::lock_guard<std::mutex> lock(pin->mtx);
                    pin->is_output = value.value("is_output", false);
                    pin->target_output_state = value.value("target_output_state", false);
                }
            }
        }
        std::cout << "[CONFIG] Configuration loaded from " << config_path << std::endl;
        return true;
    } catch (const std::exception& e) {
        std::cerr << "[CONFIG] Error parsing JSON: " << e.what() << std::endl;
        return false;
    }
}

void ConfigManager::save(const std::map<int, std::shared_ptr<PinState>>& registry, int timeout_ms) {
    json j;
    {
        std::ifstream existing(config_path);
        if (existing.is_open()) {
            try { existing >> j; } catch (...) { j = json::object(); }
        }
    }
    if (!j.is_object()) j = json::object();
    j["timeout_ms"] = timeout_ms;
    j["pins"] = json::object();

    for (const auto& [phys, pin] : registry) {
        std::lock_guard<std::mutex> lock(pin->mtx);
        j["pins"][std::to_string(phys)] = {
            {"is_output", pin->is_output},
            {"target_output_state", pin->target_output_state}
        };
    }

    std::ofstream f(config_path);
    if (f.is_open()) {
        f << j.dump(2);
        std::cout << "[CONFIG] Configuration saved to " << config_path << std::endl;
    } else {
        std::cerr << "[CONFIG] Failed to save config file!" << std::endl;
    }
}

std::string ConfigManager::getSetting(const std::string& key, const std::string& defaultValue) {
    std::ifstream f(config_path);
    if (!f.is_open()) return defaultValue;
    try {
        json j;
        f >> j;
        return j.value(key, defaultValue);
    } catch (...) {
        return defaultValue;
    }
}

void ConfigManager::setSetting(const std::string& key, const std::string& value) {
    json j;
    {
        std::ifstream existing(config_path);
        if (existing.is_open()) {
            try { existing >> j; } catch (...) { j = json::object(); }
        }
    }
    if (!j.is_object()) j = json::object();
    j[key] = value;
    std::ofstream f(config_path);
    if (f.is_open()) {
        f << j.dump(2);
    }
}
