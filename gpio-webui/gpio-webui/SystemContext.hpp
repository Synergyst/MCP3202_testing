#pragma once
#include <atomic>
#include <string>
#include <mutex>
#include <memory>
#include "SharedSignalBuffer.hpp"

struct SystemContext {
    std::atomic<int> timeout_ms{5000};
    std::string config_path = "config.json";
    std::mutex config_mutex;
    std::shared_ptr<SharedSignalBuffer> signal_buffer = std::make_shared<SharedSignalBuffer>();
};
