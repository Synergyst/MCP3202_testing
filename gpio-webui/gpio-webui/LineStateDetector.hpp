#pragma once

#include "AdcSampler.hpp"
#include "Ch1817Driver.hpp"
#include "SignalProcessing.hpp"
#include "SystemContext.hpp"

#include <atomic>
#include <chrono>
#include <deque>
#include <memory>
#include <mutex>
#include <nlohmann/json.hpp>
#include <string>
#include <thread>
#include <vector>

class LineStateDetector {
public:
    enum class State {
        Unknown,
        Silence,
        Idle,
        Ringing,
        Ringback,
        DialTone,
        Busy,
        Reorder,
        RemoteHangupTone,
        DisconnectTone,
        ReceiverOffHookTone,
        HowlerTone,
        UnknownTone,
        VoiceOrAudio,
        HardwareRiActive
    };

    struct Settings {
        bool enabled = true;
        std::string region = "nanp";
        size_t analysis_window_ms = 100;
        size_t cadence_history_ms = 8000;
        size_t update_interval_ms = 50;
        double min_rms = 0.002;
        double silence_rms = 0.0008;
        bool use_ri_corroboration = true;
        bool prefer_ri_for_ring_start = true;
        double tone_detect_threshold = 0.08;
        double tone_relative_db = -24.0;
        int required_stable_windows = 3;
        int release_stable_windows = 5;
    };

    struct ToneSpec {
        std::string name;
        State state = State::Unknown;
        std::vector<double> frequencies_hz;
        double min_confidence = 0.55;
        double min_rms = 0.001;
        bool continuous = false;
    };

    struct CadenceSpec {
        State state = State::Unknown;
        std::string name;
        int on_min_ms = 0;
        int on_max_ms = 0;
        int off_min_ms = 0;
        int off_max_ms = 0;
        double cadence_bonus = 0.25;
    };

    struct RegionProfile {
        std::string id;
        std::string label;
        std::vector<ToneSpec> tones;
        std::vector<CadenceSpec> cadences;
    };

    struct Snapshot {
        bool enabled = false;
        bool running = false;
        State state = State::Unknown;
        std::string state_text = "unknown";
        double confidence = 0.0;
        double rms = 0.0;
        double peak = 0.0;
        double mean = 0.0;
        double zero_crossing_hz = 0.0;
        bool ri_available = false;
        bool ri_level = true;
        bool ri_active = false;
        bool ri_ringing = false;
        double ri_frequency_hz = 0.0;
        std::string best_tone;
        std::string region;
        std::string region_label;
        std::vector<dsp::ToneEnergy> tones;
        uint64_t analyzed_windows = 0;
        uint32_t sample_rate_hz = 0;
        size_t window_samples = 0;
        std::string status;
        std::string last_error;
        std::string last_transition;
        Settings settings;
    };

    struct AnalysisResult {
        State state = State::Unknown;
        std::string tone_name;
        double confidence = 0.0;
        double rms = 0.0;
        double peak = 0.0;
        double mean = 0.0;
        double zero_crossing_hz = 0.0;
        std::vector<dsp::ToneEnergy> tones;
        std::vector<State> active_tone_states;
        bool tonal = false;
        double best_frequency_score = 0.0;
        std::string status;
    };

    LineStateDetector(std::shared_ptr<SystemContext> context, AdcSampler* sampler, Ch1817Driver* ch1817_driver = nullptr);
    ~LineStateDetector();

    LineStateDetector(const LineStateDetector&) = delete;
    LineStateDetector& operator=(const LineStateDetector&) = delete;

    void start();
    void stop();

    Snapshot snapshot() const;
    nlohmann::json snapshotJson() const;
    Settings settings() const;
    nlohmann::json settingsJson() const;
    void updateSettings(const Settings& settings);
    void updateSettingsFromJson(const nlohmann::json& j);

    static std::string stateText(State s);
    static State stateFromText(const std::string& s);
    static nlohmann::json settingsToJson(const Settings& s);
    static Settings settingsFromJson(const nlohmann::json& j, const Settings& defaults);
    static nlohmann::json snapshotToJson(const Snapshot& s);
    static RegionProfile builtInProfile(const std::string& region);
    static AnalysisResult analyzeSamplesForTest(const std::vector<float>& samples, uint32_t sample_rate_hz, const Settings& settings, const RegionProfile& profile, bool ri_active = false);

private:
    struct CadenceSegment { bool active = false; int duration_ms = 0; };
    struct CadenceTracker {
        bool initialized = false;
        bool active = false;
        std::chrono::steady_clock::time_point last_change{};
        std::deque<CadenceSegment> segments;
    };

    void worker();
    void loadSettings();
    void saveSettingsLocked();
    void publish(const AnalysisResult& analysis, const nlohmann::json& ri_json, uint32_t sample_rate, size_t window_samples);
    AnalysisResult analyzeWindow(const std::vector<float>& samples, uint32_t sample_rate, bool ri_active);
    void updateCadence(State tone_state, bool active, const std::chrono::steady_clock::time_point& now);
    double cadenceConfidenceLocked(State state) const;
    void trimCadenceLocked(CadenceTracker& trk) const;
    static std::string nowIso8601();

    std::shared_ptr<SystemContext> context_;
    AdcSampler* sampler_ = nullptr;
    Ch1817Driver* ch1817_driver_ = nullptr;

    mutable std::mutex mtx_;
    Settings settings_;
    Snapshot state_;
    State stable_state_ = State::Unknown;
    State candidate_state_ = State::Unknown;
    int candidate_count_ = 0;
    std::vector<CadenceTracker> cadence_trackers_;

    std::atomic<bool> running_{false};
    std::thread worker_;
};
