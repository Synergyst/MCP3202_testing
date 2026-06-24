#pragma once

#include "AdcSampler.hpp"
#include "SystemContext.hpp"

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#include <nlohmann/json.hpp>

struct CallerIdSettings {
    int channel = 0;                 // 0=CH0, 1=CH1, 2=mix, -1=auto
    double mark_hz = 1200.0;
    double space_hz = 2200.0;
    double baud = 1200.0;
    size_t analysis_ms = 5000;
    bool normalize = true;
    double normalize_headroom_db = 6.0; // negative offset relative to clipping/full-scale
    double extra_gain_db = 12.0;
    bool dc_block = true;
};

struct CallerIdSnapshot {
    bool enabled = false;
    bool running = false;
    bool detected = false;
    bool checksum_ok = false;
    uint64_t total_decodes = 0;
    uint64_t last_total_frames_seen = 0;
    uint32_t sample_rate_hz = 0;
    int selected_channel = -1; // 0, 1, or 2 for mixed/auto
    double confidence = 0.0;
    double best_confidence = 0.0;
    int best_selected_channel = -1;
    std::string status;
    std::string message_type;
    std::string date_time;
    std::string date_time_raw;
    std::string number;
    std::string name;
    std::string raw_bits;
    std::string raw_bytes_hex;
    std::string best_raw_bits;
    std::string best_raw_bytes_hex;
    std::string best_status;
    std::string best_update;
    std::string last_update;
    std::string last_error;
    CallerIdSettings settings;
};

class CallerIdDetector {
public:
    CallerIdDetector(AdcSampler* sampler, std::shared_ptr<SystemContext> context);
    ~CallerIdDetector();

    CallerIdDetector(const CallerIdDetector&) = delete;
    CallerIdDetector& operator=(const CallerIdDetector&) = delete;

    void start();
    void stop();
    CallerIdSnapshot snapshot() const;
    nlohmann::json snapshotJson() const;
    CallerIdSettings settings() const;
    nlohmann::json settingsJson() const;
    void updateSettings(const CallerIdSettings& settings);
    void updateSettingsFromJson(const nlohmann::json& j);

public:
    struct DecodeCandidate {
        bool valid = false;
        bool checksum_ok = false;
        double confidence = 0.0;
        int selected_channel = -1;
        std::string status;
        std::string message_type;
        std::string date_time;
        std::string date_time_raw;
        std::string number;
        std::string name;
        std::string raw_bits;
        std::string raw_bytes_hex;
        std::vector<uint8_t> bytes;
    };

private:
    void worker();
    DecodeCandidate analyzeRecent();
    void publishCandidate(const DecodeCandidate& c, uint32_t sample_rate, uint64_t frames_seen);
    void loadLastLogged();
    void saveLastLoggedLocked();
    void saveSettingsLocked();

    static nlohmann::json toJson(const CallerIdSnapshot& s);
    static nlohmann::json settingsToJson(const CallerIdSettings& s);
    static CallerIdSettings settingsFromJson(const nlohmann::json& j, const CallerIdSettings& defaults = CallerIdSettings());
    static std::string nowIso8601();

    AdcSampler* sampler_ = nullptr;
    std::shared_ptr<SystemContext> context_;
    mutable std::mutex mtx_;
    CallerIdSnapshot state_;
    CallerIdSettings settings_;
    std::atomic<bool> running_{false};
    std::thread worker_;
};
