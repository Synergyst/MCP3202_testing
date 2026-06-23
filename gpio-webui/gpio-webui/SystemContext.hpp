#pragma once
#include <atomic>
#include <string>
#include <mutex>

struct SystemContext {
    std::atomic<int> timeout_ms{5000};
    std::string config_path = "config.json";
    std::mutex config_mutex;
};
