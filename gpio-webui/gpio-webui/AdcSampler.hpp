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
    // "mcp3202-spidev" or "rp2040"
    std::string adc_source;
    uint32_t sample_rate_hz = 8000;
    uint32_t measured_sample_rate_hz = 8000;
    uint32_t lifetime_sample_rate_hz = 8000;
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
    // SPI/MCP3202 mode fields
    bool realtime_requested = false;
    bool realtime_active = false;
    int realtime_priority = 0;
    int cpu_affinity = -1;
    std::string scheduler_status;
    // RP2040 stream diagnostics
    bool rp2040_connected = false;
    uint64_t rp2040_packets_ok = 0;
    uint64_t rp2040_packets_crc_bad = 0;
    uint64_t rp2040_sequence_gaps = 0;
    uint32_t rp2040_firmware_lost_frames = 0;
    uint32_t rp2040_firmware_flags = 0;
    std::string rp2040_dev;
    uint32_t rp2040_declared_rate_hz = 0;

    size_t valid_samples = 0;
    size_t history_capacity_samples = 0;
    std::string last_error;
    std::array<std::vector<uint16_t>, 2> samples;
};

class AdcSampler {
public:
    struct Config {
        bool enabled = true;
        // "mcp3202-spidev" (default) or "rp2040"
        std::string adc_source = "mcp3202-spidev";
        uint32_t sample_rate_hz = 8000;
        size_t history_samples = 8000 * 30;
        double vref = 3.3;
        // MCP3202 spidev options
        bool realtime = false;
        int realtime_priority = 10;
        int cpu_affinity = -1;
        MCP3202::Config adc;
        // RP2040 USB CDC options
        std::string rp2040_dev = "/dev/ttyACM0";
    };

    AdcSampler();
    explicit AdcSampler(Config config);
    ~AdcSampler();

    AdcSampler(const AdcSampler&) = delete;
    AdcSampler& operator=(const AdcSampler&) = delete;

    void start();
    void stop();
    void setSampleRate(uint32_t rate);

    bool isEnabled() const;
    Config config() const;
    void updateConfig(Config new_config);
    AdcScopeData status() const;
    AdcScopeData snapshot(size_t max_points = 1600) const;
    AdcScopeData recent(size_t frames) const;

private:
    // MCP3202/spidev sampling loop
    void workerSpidev();
    // RP2040 USB CDC reader loop
    void workerRp2040();
    bool sendRp2040RateCommandLocked(int fd, uint32_t rate, std::string& error);

    void configureWorkerScheduling();
    void fillStatusLocked(AdcScopeData& data) const;
    void pushFrameLocked(uint16_t ch0, uint16_t ch1,
                         const std::chrono::steady_clock::time_point& sample_time,
                         uint64_t read_us, uint64_t wait_us);
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

    // RP2040 diagnostics (protected by mtx_)
    bool rp2040_connected_ = false;
    uint64_t rp2040_packets_ok_ = 0;
    uint64_t rp2040_packets_crc_bad_ = 0;
    uint64_t rp2040_sequence_gaps_ = 0;
    uint32_t rp2040_firmware_lost_frames_ = 0;
    uint32_t rp2040_firmware_flags_ = 0;
    uint32_t rp2040_declared_rate_hz_ = 0;
    bool rp2040_have_last_seq_ = false;
    uint32_t rp2040_last_seq_ = 0;
    int rp2040_fd_ = -1;
    uint32_t rp2040_pending_rate_hz_ = 0;

    std::atomic<bool> running_{false};
    std::thread worker_;
    std::unique_ptr<MCP3202> adc_;
};
