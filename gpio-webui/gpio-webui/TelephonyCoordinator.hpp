#pragma once

#include "CallerIdDetector.hpp"
#include "Ch1817Driver.hpp"
#include "LineStateDetector.hpp"
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

class TelephonyCoordinator {
public:
    enum class State {
        Unknown,
        OnHookIdle,
        Ringing,
        CallerIdPending,
        CallerIdReceiving,
        ReadyToAnswer,
        AutoAnswerArmed,
        OffHook,
        DialTone,
        Busy,
        Reorder,
        Ringback,
        RemoteHangupLikely,
        DisconnectTone,
        ReceiverOffHookWarning,
        AutoHangupPending,
        HungUp,
        Silence,
        Error
    };

    struct Settings {
        bool enabled = true;
        bool caller_id_before_auto_answer = true;
        int min_rings_before_answer = 1;
        int max_caller_id_wait_ms = 7000;
        bool auto_answer_enabled = false;
        int auto_answer_delay_ms = 0;
        bool require_line_state_ringing = false;
        bool allow_ri_only_ring = true;
        int remote_hangup_silence_ms = 5000;
        bool auto_hangup_enabled = true;
        bool auto_hangup_on_busy_after_call = true;
        bool auto_hangup_on_reorder_after_call = true;
        bool auto_hangup_on_receiver_offhook_warning = true;
        bool auto_hangup_on_unknown_tone = true;
        int auto_hangup_after_disconnect_ms = 1500;
        int auto_hangup_after_reorder_ms = 3000;
        int auto_hangup_after_warning_ms = 500;
        int auto_hangup_after_silence_ms = 5000;
        int auto_hangup_after_unknown_tone_ms = 3000;
        int update_interval_ms = 100;
    };

    struct Inputs {
        bool ch_available = false;
        bool offhook = false;
        bool ri_level = true;
        bool ri_active = false;
        bool ri_ringing = false;
        double ri_frequency_hz = 0.0;
        std::string ch_status;
        std::string ch_error;

        bool line_available = false;
        std::string line_state = "unknown";
        double line_confidence = 0.0;
        std::string line_status;
        int line_state_duration_ms = 0;

        bool caller_available = false;
        bool caller_detected = false;
        bool caller_checksum_ok = false;
        std::string caller_status;
        std::string caller_number;
        std::string caller_name;
    };

    struct Decision {
        State state = State::Unknown;
        bool incoming_ring = false;
        bool safe_to_answer = false;
        bool should_answer = false;
        bool should_hangup = false;
        bool caller_id_waiting = false;
        std::string answer_block_reason;
        std::string hangup_reason;
        std::string status;
    };

    struct Snapshot {
        bool enabled = false;
        bool running = false;
        State state = State::Unknown;
        std::string state_text = "unknown";
        bool hook_offhook = false;
        bool safe_to_answer = false;
        bool auto_answer_armed = false;
        bool auto_hangup_armed = false;
        bool caller_id_waiting = false;
        std::string answer_block_reason;
        std::string hangup_reason;
        std::string last_auto_hangup_reason;
        std::string last_onhook_at;
        uint64_t ring_count = 0;
        bool in_ring_burst = false;
        bool in_silent_gap = false;
        std::string first_ring_at;
        std::string last_ring_at;
        std::string last_transition;
        std::string status;
        std::string last_error;
        std::vector<std::string> events;
        Inputs inputs;
        Settings settings;
    };

    TelephonyCoordinator(std::shared_ptr<SystemContext> context,
                         Ch1817Driver* ch1817_driver,
                         LineStateDetector* line_state_detector,
                         CallerIdDetector* caller_id_detector);
    ~TelephonyCoordinator();

    TelephonyCoordinator(const TelephonyCoordinator&) = delete;
    TelephonyCoordinator& operator=(const TelephonyCoordinator&) = delete;

    void start();
    void stop();

    Snapshot snapshot() const;
    nlohmann::json snapshotJson() const;
    Settings settings() const;
    nlohmann::json settingsJson() const;
    void updateSettings(const Settings& settings);
    void updateSettingsFromJson(const nlohmann::json& j);

    static std::string stateText(State s);
    static nlohmann::json settingsToJson(const Settings& s);
    static Settings settingsFromJson(const nlohmann::json& j, const Settings& defaults);
    static nlohmann::json snapshotToJson(const Snapshot& s);
    static Decision decide(const Settings& settings,
                           const Inputs& inputs,
                           uint64_t ring_count,
                           int ms_since_first_ring,
                           int ms_since_last_ring,
                           bool caller_wait_timed_out);

private:
    void worker();
    void loadSettings();
    void saveSettingsLocked();
    Inputs collectInputs() const;
    void updateRingLifecycleLocked(const Inputs& in, const std::chrono::steady_clock::time_point& now);
    void publishLocked(const Inputs& in, const Decision& d);
    void addEventLocked(const std::string& event);
    static std::string nowIso8601();
    static int msSince(const std::chrono::steady_clock::time_point& t, const std::chrono::steady_clock::time_point& now);

    std::shared_ptr<SystemContext> context_;
    Ch1817Driver* ch1817_driver_ = nullptr;
    LineStateDetector* line_state_detector_ = nullptr;
    CallerIdDetector* caller_id_detector_ = nullptr;

    mutable std::mutex mtx_;
    Settings settings_;
    Snapshot state_;
    State stable_state_ = State::Unknown;
    std::chrono::steady_clock::time_point first_ring_time_{};
    std::chrono::steady_clock::time_point last_ring_time_{};
    std::chrono::steady_clock::time_point offhook_silence_start_{};
    std::chrono::steady_clock::time_point line_state_start_{};
    std::chrono::steady_clock::time_point offhook_start_{};
    std::string last_line_state_ = "unknown";
    bool had_ring_ = false;
    bool prev_ring_signal_ = false;
    uint64_t ring_count_ = 0;
    std::deque<std::string> events_;

    std::atomic<bool> running_{false};
    std::thread worker_;
};
