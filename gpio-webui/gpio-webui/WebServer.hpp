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
#include "CallerIdDetector.hpp"
#include "Ch1817Driver.hpp"
#include "LineStateDetector.hpp"
#include "TelephonyCoordinator.hpp"
#include "TelephonyDiagnostics.hpp"

class WebServer {
public:
    WebServer(std::map<int, std::shared_ptr<PinState>>& registry, 
              ConfigManager& config, 
              GpioManager& gpio,
              std::shared_ptr<SystemContext> context,
              AdcSampler* adc_sampler = nullptr,
              CallerIdDetector* caller_id_detector = nullptr,
              Ch1817Driver* ch1817_driver = nullptr,
              LineStateDetector* line_state_detector = nullptr,
              TelephonyCoordinator* telephony_coordinator = nullptr,
              TelephonyDiagnostics* telephony_diagnostics = nullptr,
              std::set<int> reserved_bcm_pins = {});
    void listen(const std::string& host, int port);
    void update_adc_config(const std::string& source, const std::string& dev);

private:
    void setup_routes();
    std::string serialize_state();
    std::string serialize_adc_scope(size_t max_points, const std::string& view = "raw", const std::string& effects_csv = "");
    std::string build_adc_wav(const std::string& mode, size_t duration_ms, const std::string& effects_csv, const std::string& codec,
                              std::string& filename, std::string& content_type, size_t& requested_ms, size_t& actual_ms,
                              size_t& available_ms, bool& truncated);

    httplib::Server svr;
    std::map<int, std::shared_ptr<PinState>>& registry;
    ConfigManager& config_mgr;
    GpioManager& gpio_mgr;
    std::shared_ptr<SystemContext> context;
    AdcSampler* adc_sampler;
    CallerIdDetector* caller_id_detector;
    Ch1817Driver* ch1817_driver;
    LineStateDetector* line_state_detector;
    TelephonyCoordinator* telephony_coordinator;
    TelephonyDiagnostics* telephony_diagnostics;
    std::set<int> reserved_bcm_pins;
};
