#pragma once
#include "httplib.h"
#include <map>
#include <memory>
#include <set>
#include "PinConfig.hpp"
#include "ConfigManager.hpp"
#include "GpioManager.hpp"
#include "SystemContext.hpp"
#include "AdcSampler.hpp"

class WebServer {
public:
    WebServer(std::map<int, std::shared_ptr<PinState>>& registry, 
              ConfigManager& config, 
              GpioManager& gpio,
              std::shared_ptr<SystemContext> context,
              AdcSampler* adc_sampler = nullptr,
              std::set<int> reserved_bcm_pins = {});
    void listen(const std::string& host, int port);

private:
    void setup_routes();
    std::string serialize_state();
    std::string serialize_adc_scope(size_t max_points);

    httplib::Server svr;
    std::map<int, std::shared_ptr<PinState>>& registry;
    ConfigManager& config_mgr;
    GpioManager& gpio_mgr;
    std::shared_ptr<SystemContext> context;
    AdcSampler* adc_sampler;
    std::set<int> reserved_bcm_pins;
};
