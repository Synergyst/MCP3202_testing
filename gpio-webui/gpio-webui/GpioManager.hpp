#pragma once
#include <map>
#include <memory>
#include <thread>
#include <atomic>
#include <vector>
#include <set>
#include <gpiod.hpp>
#include "PinConfig.hpp"

class GpioManager {
public:
    GpioManager(std::map<int, std::shared_ptr<PinState>>& registry,
                std::set<int> reserved_bcm_pins = {});
    ~GpioManager();

    void start(int timeout_ms);
    void stop();
    void updateTimeout(int new_timeout);

private:
    void worker_thread(int phys, int bcm, std::shared_ptr<PinState> state);

    std::map<int, std::shared_ptr<PinState>>& registry;
    std::set<int> reserved_bcm_pins;
    std::atomic<bool> running{false};
    std::atomic<int> timeout_ms{5000};
    std::vector<std::thread> workers;
};
