#pragma once

#include "SystemContext.hpp"
#include "McuPeripheral.hpp"
#include <atomic>
#include <chrono>
#include <memory>
#include <mutex>
#include <nlohmann/json.hpp>
#include <string>
#include <thread>
#include <gpiod.hpp>

class AdcSampler;

struct Ch1817Settings {
    bool enabled = true;
    int offhook_phys = 32;
    int ri_phys = 40;
    bool offhook = false;
    bool auto_answer_enabled = false;
    int auto_answer_delay_ms = 0;
};

class Ch1817Driver {
public:
    explicit Ch1817Driver(std::shared_ptr<SystemContext> context, AdcSampler* adc_sampler = nullptr);
    ~Ch1817Driver();

    void start();
    void stop();

    Ch1817Settings settings() const;
    nlohmann::json settingsJson() const;
    nlohmann::json snapshotJson() const;
    void updateFromJson(const nlohmann::json& j);
    void setOffhook(bool offhook);
    void setMcuPeripheralConfig(const McuPeripheralConfig& config);
    void setAdcSampler(AdcSampler* adc_sampler);

    int offhookBcm() const;
    int riBcm() const;
    std::string validationHelp() const;

    static Ch1817Settings settingsFromJson(const nlohmann::json& j, const Ch1817Settings& defaults = Ch1817Settings());
    static nlohmann::json settingsToJson(const Ch1817Settings& s);
    static void validate(const Ch1817Settings& s);

private:
    void worker();
    void load();
    void saveLocked();
    void updateMcuTelephonyStateLocked(const McuPeripheralSnapshot& snap, const std::chrono::steady_clock::time_point& now);

    std::shared_ptr<SystemContext> context_;
    AdcSampler* adc_sampler_ = nullptr;
    mutable std::mutex mtx_;
    Ch1817Settings settings_;
    bool ri_level_ = true;
    bool ringing_ = false;
    double ri_frequency_hz_ = 0.0;
    uint64_t ri_edges_ = 0;
    std::string status_ = "stopped";
    std::string last_error_;
    McuPeripheralConfig mcu_periph_config_;
    bool using_mcu_ri_ = false;
    bool using_mcu_oh_ = false;
    bool mcu_ri_raw_ = false;
    bool mcu_ri_logical_ = false;
    bool mcu_oh_raw_ = false;
    bool mcu_oh_logical_ = false;
    uint32_t mcu_ri_transitions_ = 0;
    uint32_t mcu_oh_transitions_ = 0;
    std::chrono::steady_clock::time_point ring_start_{};
    std::chrono::steady_clock::time_point last_edge_{};
    std::chrono::steady_clock::time_point freq_window_start_{};
    uint64_t freq_edges_ = 0;

    std::atomic<bool> running_{false};
    std::thread worker_;
};
