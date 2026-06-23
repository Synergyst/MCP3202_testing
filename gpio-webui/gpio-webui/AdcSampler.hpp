#pragma once

#include "MCP3202.hpp"

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

struct AdcScopeData {
    bool enabled = false;
    bool running = false;
    bool healthy = false;
    bool bitbang = false;
    uint32_t sample_rate_hz = 8000; // requested/nominal complete two-channel frames per second
    uint32_t measured_sample_rate_hz = 8000; // measured effective ring-buffer frame rate
    std::array<uint16_t, 2> latest_raw{{0, 0}};
    std::array<double, 2> latest_volts{{0.0, 0.0}};
    uint64_t total_frames = 0;
    uint64_t dropped_reads = 0;
    std::string last_error;
    std::array<std::vector<uint16_t>, 2> samples;
};

class AdcSampler {
public:
    struct Config {
        bool enabled = true;
        uint32_t sample_rate_hz = 8000; // complete two-channel frames per second
        size_t history_samples = 8000 * 4; // four seconds of per-channel history
        double vref = 3.3;
        MCP3202::Config adc;
    };

    AdcSampler();
    explicit AdcSampler(Config config);
    ~AdcSampler();

    AdcSampler(const AdcSampler&) = delete;
    AdcSampler& operator=(const AdcSampler&) = delete;

    void start();
    void stop();

    bool isEnabled() const;
    AdcScopeData snapshot(size_t max_points = 1600) const;
    AdcScopeData recent(size_t frames) const;

private:
    void worker();
    std::vector<uint16_t> copyRecentLocked(int channel, size_t max_points) const;
    std::vector<uint16_t> copyRecentExactLocked(int channel, size_t frames) const;

    Config config_;
    mutable std::mutex mtx_;
    std::array<std::vector<uint16_t>, 2> ring_;
    size_t write_index_ = 0;
    size_t valid_samples_ = 0;
    std::array<uint16_t, 2> latest_raw_{{0, 0}};
    uint64_t total_frames_ = 0;
    uint64_t dropped_reads_ = 0;
    uint32_t measured_sample_rate_hz_ = 0;
    std::chrono::steady_clock::time_point first_sample_time_{};
    std::chrono::steady_clock::time_point last_sample_time_{};
    bool healthy_ = false;
    std::string last_error_;

    std::atomic<bool> running_{false};
    std::thread worker_;
    std::unique_ptr<MCP3202> adc_;
};
