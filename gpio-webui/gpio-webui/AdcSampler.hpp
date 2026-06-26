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
    uint32_t measured_sample_rate_hz = 8000; // recent/windowed effective ring-buffer frame rate
    uint32_t lifetime_sample_rate_hz = 8000; // lifetime average since first successful sample
    std::array<uint16_t, 2> latest_raw{{0, 0}};
    std::array<double, 2> latest_volts{{0.0, 0.0}};
    uint64_t total_frames = 0;
    uint64_t dropped_reads = 0;
    uint64_t overruns = 0;
    uint64_t max_overrun_us = 0;
    uint64_t avg_frame_read_us = 0;
    uint64_t max_frame_read_us = 0;
    uint64_t avg_mutex_wait_us = 0;
    uint64_t max_mutex_wait_us = 0;
    uint64_t avg_mutex_hold_us = 0;
    uint64_t max_mutex_hold_us = 0;
    uint64_t snapshot_count = 0;
    uint64_t snapshot_samples_copied = 0;
    bool realtime_requested = false;
    bool realtime_active = false;
    int realtime_priority = 0;
    int cpu_affinity = -1;
    std::string scheduler_status;
    size_t valid_samples = 0;
    size_t history_capacity_samples = 0;
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
        bool realtime = false;       // optional SCHED_FIFO for ADC worker
        int realtime_priority = 10;  // modest RT priority when realtime=true
        int cpu_affinity = -1;       // -1 = do not pin, otherwise Linux CPU index
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
    AdcScopeData status() const;
    AdcScopeData snapshot(size_t max_points = 1600) const;
    AdcScopeData recent(size_t frames) const;

private:
    void worker();
    void configureWorkerScheduling();
    void fillStatusLocked(AdcScopeData& data) const;
    std::vector<uint16_t> copyRecentDecimatedLocked(int channel, size_t max_points) const;
    std::vector<uint16_t> copyRecentExactLocked(int channel, size_t frames) const;
    static uint64_t ema(uint64_t old_value, uint64_t sample, uint32_t weight = 31);

    Config config_;
    mutable std::mutex mtx_;
    std::array<std::vector<uint16_t>, 2> ring_;
    size_t write_index_ = 0;
    size_t valid_samples_ = 0;
    std::array<uint16_t, 2> latest_raw_{{0, 0}};
    uint64_t total_frames_ = 0;
    uint64_t dropped_reads_ = 0;
    uint64_t overruns_ = 0;
    uint64_t max_overrun_us_ = 0;
    uint32_t measured_sample_rate_hz_ = 0;
    uint32_t lifetime_sample_rate_hz_ = 0;
    std::chrono::steady_clock::time_point first_sample_time_{};
    std::chrono::steady_clock::time_point last_sample_time_{};
    std::chrono::steady_clock::time_point rate_window_start_{};
    uint64_t rate_window_frames_ = 0;
    uint64_t avg_frame_read_us_ = 0;
    uint64_t max_frame_read_us_ = 0;
    uint64_t avg_mutex_wait_us_ = 0;
    uint64_t max_mutex_wait_us_ = 0;
    uint64_t avg_mutex_hold_us_ = 0;
    uint64_t max_mutex_hold_us_ = 0;
    mutable uint64_t snapshot_count_ = 0;
    mutable uint64_t snapshot_samples_copied_ = 0;
    bool realtime_active_ = false;
    std::string scheduler_status_;
    bool healthy_ = false;
    std::string last_error_;

    std::atomic<bool> running_{false};
    std::thread worker_;
    std::unique_ptr<MCP3202> adc_;
};
