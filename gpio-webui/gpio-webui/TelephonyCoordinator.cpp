#include "TelephonyCoordinator.hpp"

#include <algorithm>
#include <ctime>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>

using json = nlohmann::json;

namespace {
int clampInt(int v, int lo, int hi) { return std::max(lo, std::min(hi, v)); }
std::string lower(std::string s) { std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c){ return static_cast<char>(std::tolower(c)); }); return s; }
bool lineIsRinging(const std::string& s) { const auto v = lower(s); return v == "ringing" || v == "hardware_ri_active"; }
bool lineIsSilence(const std::string& s) { const auto v = lower(s); return v == "silence" || v == "idle"; }
bool lineIsDisconnectTone(const std::string& s) { const auto v = lower(s); return v == "remote_hangup_tone" || v == "disconnect_tone"; }
bool lineIsWarningTone(const std::string& s) { const auto v = lower(s); return v == "receiver_offhook_tone" || v == "howler_tone"; }
bool lineIsUnknownTone(const std::string& s) { return lower(s) == "unknown_tone"; }
}

TelephonyCoordinator::TelephonyCoordinator(std::shared_ptr<SystemContext> context,
                                           Ch1817Driver* ch1817_driver,
                                           LineStateDetector* line_state_detector,
                                           CallerIdDetector* caller_id_detector)
    : context_(std::move(context)), ch1817_driver_(ch1817_driver), line_state_detector_(line_state_detector), caller_id_detector_(caller_id_detector) {
    loadSettings();
    state_.enabled = settings_.enabled;
    state_.settings = settings_;
}

TelephonyCoordinator::~TelephonyCoordinator() { stop(); }

void TelephonyCoordinator::start() {
    if (running_) return;
    { std::lock_guard<std::mutex> lock(mtx_); state_.enabled = settings_.enabled; state_.running = false; }
    if (!settings_.enabled) return;
    running_ = true;
    worker_ = std::thread(&TelephonyCoordinator::worker, this);
}

void TelephonyCoordinator::stop() {
    running_ = false;
    if (worker_.joinable()) worker_.join();
}

TelephonyCoordinator::Snapshot TelephonyCoordinator::snapshot() const {
    std::lock_guard<std::mutex> lock(mtx_);
    auto s = state_;
    s.settings = settings_;
    s.events.assign(events_.begin(), events_.end());
    return s;
}

json TelephonyCoordinator::snapshotJson() const { return snapshotToJson(snapshot()); }

TelephonyCoordinator::Settings TelephonyCoordinator::settings() const { std::lock_guard<std::mutex> lock(mtx_); return settings_; }
json TelephonyCoordinator::settingsJson() const { return settingsToJson(settings()); }

void TelephonyCoordinator::updateSettings(const Settings& settings) {
    std::lock_guard<std::mutex> lock(mtx_);
    settings_ = settings;
    state_.settings = settings_;
    state_.enabled = settings_.enabled;
    addEventLocked("settings updated");
    saveSettingsLocked();
}

void TelephonyCoordinator::updateSettingsFromJson(const json& j) {
    Settings defaults;
    { std::lock_guard<std::mutex> lock(mtx_); defaults = settings_; }
    updateSettings(settingsFromJson(j, defaults));
}

std::string TelephonyCoordinator::stateText(State s) {
    switch (s) {
        case State::Unknown: return "unknown";
        case State::OnHookIdle: return "on_hook_idle";
        case State::Ringing: return "ringing";
        case State::CallerIdPending: return "caller_id_pending";
        case State::CallerIdReceiving: return "caller_id_receiving";
        case State::ReadyToAnswer: return "ready_to_answer";
        case State::AutoAnswerArmed: return "auto_answer_armed";
        case State::OffHook: return "off_hook";
        case State::DialTone: return "dial_tone";
        case State::Busy: return "busy";
        case State::Reorder: return "reorder";
        case State::Ringback: return "ringback";
        case State::RemoteHangupLikely: return "remote_hangup_likely";
        case State::DisconnectTone: return "disconnect_tone";
        case State::ReceiverOffHookWarning: return "receiver_offhook_warning";
        case State::AutoHangupPending: return "auto_hangup_pending";
        case State::HungUp: return "hung_up";
        case State::Silence: return "silence";
        case State::Error: return "error";
    }
    return "unknown";
}

json TelephonyCoordinator::settingsToJson(const Settings& s) {
    return {{"enabled", s.enabled}, {"caller_id_before_auto_answer", s.caller_id_before_auto_answer},
            {"min_rings_before_answer", s.min_rings_before_answer}, {"max_caller_id_wait_ms", s.max_caller_id_wait_ms},
            {"auto_answer_enabled", s.auto_answer_enabled}, {"auto_answer_delay_ms", s.auto_answer_delay_ms},
            {"require_line_state_ringing", s.require_line_state_ringing}, {"allow_ri_only_ring", s.allow_ri_only_ring},
            {"remote_hangup_silence_ms", s.remote_hangup_silence_ms},
            {"auto_hangup_enabled", s.auto_hangup_enabled},
            {"auto_hangup_on_busy_after_call", s.auto_hangup_on_busy_after_call},
            {"auto_hangup_on_reorder_after_call", s.auto_hangup_on_reorder_after_call},
            {"auto_hangup_on_receiver_offhook_warning", s.auto_hangup_on_receiver_offhook_warning},
            {"auto_hangup_on_unknown_tone", s.auto_hangup_on_unknown_tone},
            {"auto_hangup_after_disconnect_ms", s.auto_hangup_after_disconnect_ms},
            {"auto_hangup_after_reorder_ms", s.auto_hangup_after_reorder_ms},
            {"auto_hangup_after_warning_ms", s.auto_hangup_after_warning_ms},
            {"auto_hangup_after_silence_ms", s.auto_hangup_after_silence_ms},
            {"auto_hangup_after_unknown_tone_ms", s.auto_hangup_after_unknown_tone_ms},
            {"update_interval_ms", s.update_interval_ms}};
}

TelephonyCoordinator::Settings TelephonyCoordinator::settingsFromJson(const json& j, const Settings& defaults) {
    Settings s = defaults;
    if (!j.is_object()) return s;
    s.enabled = j.value("enabled", s.enabled);
    s.caller_id_before_auto_answer = j.value("caller_id_before_auto_answer", s.caller_id_before_auto_answer);
    s.min_rings_before_answer = clampInt(j.value("min_rings_before_answer", s.min_rings_before_answer), 0, 20);
    s.max_caller_id_wait_ms = clampInt(j.value("max_caller_id_wait_ms", s.max_caller_id_wait_ms), 0, 60000);
    s.auto_answer_enabled = j.value("auto_answer_enabled", s.auto_answer_enabled);
    s.auto_answer_delay_ms = clampInt(j.value("auto_answer_delay_ms", s.auto_answer_delay_ms), 0, 600000);
    s.require_line_state_ringing = j.value("require_line_state_ringing", s.require_line_state_ringing);
    s.allow_ri_only_ring = j.value("allow_ri_only_ring", s.allow_ri_only_ring);
    s.remote_hangup_silence_ms = clampInt(j.value("remote_hangup_silence_ms", s.remote_hangup_silence_ms), 1000, 600000);
    s.auto_hangup_enabled = j.value("auto_hangup_enabled", s.auto_hangup_enabled);
    s.auto_hangup_on_busy_after_call = j.value("auto_hangup_on_busy_after_call", s.auto_hangup_on_busy_after_call);
    s.auto_hangup_on_reorder_after_call = j.value("auto_hangup_on_reorder_after_call", s.auto_hangup_on_reorder_after_call);
    s.auto_hangup_on_receiver_offhook_warning = j.value("auto_hangup_on_receiver_offhook_warning", s.auto_hangup_on_receiver_offhook_warning);
    s.auto_hangup_on_unknown_tone = j.value("auto_hangup_on_unknown_tone", s.auto_hangup_on_unknown_tone);
    s.auto_hangup_after_disconnect_ms = clampInt(j.value("auto_hangup_after_disconnect_ms", s.auto_hangup_after_disconnect_ms), 100, 600000);
    s.auto_hangup_after_reorder_ms = clampInt(j.value("auto_hangup_after_reorder_ms", s.auto_hangup_after_reorder_ms), 100, 600000);
    s.auto_hangup_after_warning_ms = clampInt(j.value("auto_hangup_after_warning_ms", s.auto_hangup_after_warning_ms), 100, 600000);
    s.auto_hangup_after_silence_ms = clampInt(j.value("auto_hangup_after_silence_ms", s.auto_hangup_after_silence_ms), 100, 600000);
    s.auto_hangup_after_unknown_tone_ms = clampInt(j.value("auto_hangup_after_unknown_tone_ms", s.auto_hangup_after_unknown_tone_ms), 100, 600000);
    s.update_interval_ms = clampInt(j.value("update_interval_ms", s.update_interval_ms), 25, 5000);
    return s;
}

json TelephonyCoordinator::snapshotToJson(const Snapshot& s) {
    json events = json::array(); for (const auto& e : s.events) events.push_back(e);
    return {{"enabled", s.enabled}, {"running", s.running}, {"state", stateText(s.state)}, {"state_text", s.state_text},
            {"hook_offhook", s.hook_offhook}, {"safe_to_answer", s.safe_to_answer}, {"auto_answer_armed", s.auto_answer_armed},
            {"auto_hangup_armed", s.auto_hangup_armed}, {"caller_id_waiting", s.caller_id_waiting},
            {"answer_block_reason", s.answer_block_reason}, {"hangup_reason", s.hangup_reason},
            {"last_auto_hangup_reason", s.last_auto_hangup_reason}, {"last_onhook_at", s.last_onhook_at}, {"ring_count", s.ring_count},
            {"in_ring_burst", s.in_ring_burst}, {"in_silent_gap", s.in_silent_gap}, {"first_ring_at", s.first_ring_at},
            {"last_ring_at", s.last_ring_at}, {"last_transition", s.last_transition}, {"status", s.status}, {"last_error", s.last_error},
            {"events", events},
            {"inputs", {{"ch_available", s.inputs.ch_available}, {"offhook", s.inputs.offhook}, {"ri_level", s.inputs.ri_level},
                         {"ri_active", s.inputs.ri_active}, {"ri_ringing", s.inputs.ri_ringing}, {"ri_frequency_hz", s.inputs.ri_frequency_hz},
                         {"line_available", s.inputs.line_available}, {"line_state", s.inputs.line_state}, {"line_confidence", s.inputs.line_confidence}, {"line_state_duration_ms", s.inputs.line_state_duration_ms},
                         {"caller_available", s.inputs.caller_available}, {"caller_detected", s.inputs.caller_detected},
                         {"caller_checksum_ok", s.inputs.caller_checksum_ok}, {"caller_number", s.inputs.caller_number}, {"caller_name", s.inputs.caller_name}}},
            {"settings", settingsToJson(s.settings)}};
}

TelephonyCoordinator::Decision TelephonyCoordinator::decide(const Settings& settings,
                                                            const Inputs& in,
                                                            uint64_t ring_count,
                                                            int ms_since_first_ring,
                                                            int ms_since_last_ring,
                                                            bool caller_wait_timed_out) {
    Decision d;
    d.safe_to_answer = in.ch_available && !in.ri_active;
    if (!in.ch_available) d.answer_block_reason = "CH1817 driver unavailable";
    else if (in.ri_active) d.answer_block_reason = "RI is active LOW; waiting for RI HIGH safe interval";

    const bool ri_ring = in.ri_active || in.ri_ringing;
    const bool dsp_ring = in.line_available && lineIsRinging(in.line_state) && in.line_confidence >= 0.45;
    d.incoming_ring = (!settings.require_line_state_ringing && settings.allow_ri_only_ring && ri_ring) || dsp_ring;

    if (in.offhook) {
        const auto ls = lower(in.line_state);
        d.status = "off-hook; interpreting RCV audio state";
        if (ls == "dial_tone") d.state = State::DialTone;
        else if (ls == "busy") d.state = State::Busy;
        else if (ls == "reorder") d.state = State::Reorder;
        else if (ls == "ringback") d.state = State::Ringback;
        else if (lineIsDisconnectTone(ls)) d.state = State::DisconnectTone;
        else if (lineIsWarningTone(ls)) d.state = State::ReceiverOffHookWarning;
        else if (lineIsUnknownTone(ls)) d.state = State::DisconnectTone;
        else if (lineIsSilence(ls) && in.line_state_duration_ms >= settings.auto_hangup_after_silence_ms) d.state = State::RemoteHangupLikely;
        else d.state = State::OffHook;

        if (settings.auto_hangup_enabled) {
            int threshold = 0;
            if (lineIsDisconnectTone(ls)) { threshold = settings.auto_hangup_after_disconnect_ms; d.hangup_reason = "disconnect/remote-hangup tone detected"; }
            else if (lineIsWarningTone(ls) && settings.auto_hangup_on_receiver_offhook_warning) { threshold = settings.auto_hangup_after_warning_ms; d.hangup_reason = "receiver-off-hook/howler warning tone detected"; }
            else if (lineIsUnknownTone(ls) && settings.auto_hangup_on_unknown_tone) { threshold = settings.auto_hangup_after_unknown_tone_ms; d.hangup_reason = "unknown tonal ATA warning detected"; }
            else if (ls == "busy" && settings.auto_hangup_on_busy_after_call) { threshold = settings.auto_hangup_after_disconnect_ms; d.hangup_reason = "busy tone while off-hook"; }
            else if (ls == "reorder" && settings.auto_hangup_on_reorder_after_call) { threshold = settings.auto_hangup_after_reorder_ms; d.hangup_reason = "reorder tone while off-hook"; }
            else if (lineIsSilence(ls)) { threshold = settings.auto_hangup_after_silence_ms; d.hangup_reason = "sustained silence while off-hook"; }
            if (threshold > 0) {
                if (in.line_state_duration_ms >= threshold) {
                    d.should_hangup = true;
                    d.state = State::AutoHangupPending;
                    d.status = "auto-hangup threshold reached: " + d.hangup_reason;
                } else {
                    d.status = "auto-hangup armed: " + d.hangup_reason + " (" + std::to_string(in.line_state_duration_ms) + "/" + std::to_string(threshold) + " ms)";
                }
            }
        }
        return d;
    }

    if (d.incoming_ring) {
        const bool enough_rings = static_cast<int>(ring_count) >= settings.min_rings_before_answer;
        const bool delay_elapsed = ms_since_first_ring >= settings.auto_answer_delay_ms;
        d.caller_id_waiting = settings.caller_id_before_auto_answer && !in.caller_detected && !caller_wait_timed_out;
        if (d.caller_id_waiting) d.state = State::CallerIdPending;
        else if (settings.auto_answer_enabled && enough_rings && delay_elapsed) d.state = d.safe_to_answer ? State::ReadyToAnswer : State::AutoAnswerArmed;
        else d.state = State::Ringing;
        d.should_answer = settings.auto_answer_enabled && enough_rings && delay_elapsed && !d.caller_id_waiting && d.safe_to_answer;
        d.status = d.should_answer ? "auto-answer permitted; requesting OFFHK" : (d.safe_to_answer ? "incoming ring; safe interval" : d.answer_block_reason);
        return d;
    }

    if (in.line_available && lineIsSilence(in.line_state)) d.state = State::OnHookIdle;
    else d.state = State::OnHookIdle;
    d.status = "on-hook idle";
    return d;
}

void TelephonyCoordinator::worker() {
    while (running_) {
        try {
            Settings cfg;
            { std::lock_guard<std::mutex> lock(mtx_); cfg = settings_; }
            if (!cfg.enabled) break;
            Inputs in = collectInputs();
            Decision d;
            {
                std::lock_guard<std::mutex> lock(mtx_);
                auto now = std::chrono::steady_clock::now();
                updateRingLifecycleLocked(in, now);
                if (in.offhook && offhook_start_ == std::chrono::steady_clock::time_point{}) offhook_start_ = now;
                if (!in.offhook) offhook_start_ = std::chrono::steady_clock::time_point{};
                if (in.line_state != last_line_state_) {
                    last_line_state_ = in.line_state;
                    line_state_start_ = now;
                    addEventLocked("line state -> " + in.line_state);
                }
                in.line_state_duration_ms = msSince(line_state_start_, now);
                const int first_ms = had_ring_ ? msSince(first_ring_time_, now) : 0;
                const int last_ms = had_ring_ ? msSince(last_ring_time_, now) : 0;
                const bool cid_timeout = had_ring_ && first_ms >= settings_.max_caller_id_wait_ms;
                d = decide(settings_, in, ring_count_, first_ms, last_ms, cid_timeout);
                publishLocked(in, d);
            }
            if (d.should_answer && ch1817_driver_) {
                try { ch1817_driver_->setOffhook(true); std::lock_guard<std::mutex> lock(mtx_); addEventLocked("OFFHK set HIGH by coordinator auto-answer"); }
                catch (const std::exception& e) { std::lock_guard<std::mutex> lock(mtx_); state_.last_error = e.what(); addEventLocked(std::string("auto-answer blocked: ") + e.what()); }
            }
            if (d.should_hangup && ch1817_driver_) {
                try { ch1817_driver_->setOffhook(false); std::lock_guard<std::mutex> lock(mtx_); state_.last_auto_hangup_reason = d.hangup_reason; state_.last_onhook_at = nowIso8601(); addEventLocked("OFFHK set LOW by coordinator auto-hangup: " + d.hangup_reason); }
                catch (const std::exception& e) { std::lock_guard<std::mutex> lock(mtx_); state_.last_error = e.what(); addEventLocked(std::string("auto-hangup failed: ") + e.what()); }
            }
        } catch (const std::exception& e) {
            std::lock_guard<std::mutex> lock(mtx_); state_.last_error = e.what(); state_.state = State::Error; state_.state_text = stateText(State::Error);
        }
        Settings cfg; { std::lock_guard<std::mutex> lock(mtx_); cfg = settings_; }
        for (int slept = 0; running_ && slept < cfg.update_interval_ms; slept += 25) std::this_thread::sleep_for(std::chrono::milliseconds(25));
    }
}

TelephonyCoordinator::Inputs TelephonyCoordinator::collectInputs() const {
    Inputs in;
    if (ch1817_driver_) {
        auto j = ch1817_driver_->snapshotJson();
        in.ch_available = j.value("enabled", false);
        in.offhook = j.value("offhook", false);
        in.ri_level = j.value("ri_level", true);
        in.ri_active = in.ch_available && !in.ri_level;
        in.ri_ringing = j.value("ringing", false);
        in.ri_frequency_hz = j.value("ri_frequency_hz", 0.0);
        in.ch_status = j.value("status", "");
        in.ch_error = j.value("last_error", "");
    }
    if (line_state_detector_) {
        auto j = line_state_detector_->snapshotJson();
        in.line_available = j.value("enabled", false) && j.value("running", false);
        in.line_state = j.value("state", "unknown");
        in.line_confidence = j.value("confidence", 0.0);
        in.line_status = j.value("status", "");
    }
    if (caller_id_detector_) {
        auto j = caller_id_detector_->snapshotJson();
        in.caller_available = j.value("enabled", false);
        in.caller_detected = j.value("detected", false);
        in.caller_checksum_ok = j.value("checksum_ok", false);
        in.caller_status = j.value("status", "");
        in.caller_number = j.value("number", "");
        in.caller_name = j.value("name", "");
    }
    return in;
}

void TelephonyCoordinator::updateRingLifecycleLocked(const Inputs& in, const std::chrono::steady_clock::time_point& now) {
    const bool ring_signal = in.ri_active || in.ri_ringing || (in.line_available && lineIsRinging(in.line_state) && in.line_confidence >= 0.45);
    if (ring_signal && !prev_ring_signal_) {
        if (!had_ring_ || msSince(last_ring_time_, now) > 8000) { first_ring_time_ = now; ring_count_ = 0; state_.first_ring_at = nowIso8601(); addEventLocked("new incoming ring sequence"); }
        ring_count_++; last_ring_time_ = now; had_ring_ = true; state_.last_ring_at = nowIso8601(); addEventLocked("ring burst detected");
    }
    if (!ring_signal && prev_ring_signal_) addEventLocked("ring signal entered silent/safe gap");
    prev_ring_signal_ = ring_signal;
}

void TelephonyCoordinator::publishLocked(const Inputs& in, const Decision& d) {
    if (stable_state_ != d.state) { stable_state_ = d.state; state_.last_transition = nowIso8601(); addEventLocked("state -> " + stateText(d.state)); }
    state_.enabled = settings_.enabled; state_.running = running_; state_.state = stable_state_; state_.state_text = stateText(stable_state_);
    state_.hook_offhook = in.offhook; state_.safe_to_answer = d.safe_to_answer; state_.auto_answer_armed = d.state == State::AutoAnswerArmed;
    state_.auto_hangup_armed = in.offhook && !d.hangup_reason.empty();
    state_.caller_id_waiting = d.caller_id_waiting; state_.answer_block_reason = d.answer_block_reason; state_.hangup_reason = d.hangup_reason; state_.ring_count = ring_count_;
    state_.in_ring_burst = prev_ring_signal_; state_.in_silent_gap = had_ring_ && !prev_ring_signal_ && !in.offhook;
    state_.status = d.status; state_.inputs = in; state_.settings = settings_;
}

void TelephonyCoordinator::addEventLocked(const std::string& event) {
    events_.push_back(nowIso8601() + " " + event);
    while (events_.size() > 50) events_.pop_front();
}

void TelephonyCoordinator::loadSettings() {
    if (!context_) return;
    std::lock_guard<std::mutex> cfg_lock(context_->config_mutex);
    std::ifstream f(context_->config_path);
    if (!f.is_open()) return;
    try { json j; f >> j; if (j.contains("telephony_coordinator")) settings_ = settingsFromJson(j["telephony_coordinator"], settings_); }
    catch (const std::exception& e) { std::cerr << "[TelephonyCoordinator] Config load warning: " << e.what() << std::endl; }
}

void TelephonyCoordinator::saveSettingsLocked() {
    if (!context_) return;
    std::lock_guard<std::mutex> cfg_lock(context_->config_mutex);
    json j; { std::ifstream f(context_->config_path); if (f.is_open()) { try { f >> j; } catch (...) { j = json::object(); } } }
    if (!j.is_object()) j = json::object();
    j["telephony_coordinator"] = settingsToJson(settings_);
    std::ofstream out(context_->config_path); if (out.is_open()) out << j.dump(2);
}

std::string TelephonyCoordinator::nowIso8601() {
    auto now = std::chrono::system_clock::now(); std::time_t t = std::chrono::system_clock::to_time_t(now); std::tm tm{}; localtime_r(&t, &tm);
    std::ostringstream oss; oss << std::put_time(&tm, "%Y-%m-%dT%H:%M:%S%z"); return oss.str();
}

int TelephonyCoordinator::msSince(const std::chrono::steady_clock::time_point& t, const std::chrono::steady_clock::time_point& now) {
    if (t == std::chrono::steady_clock::time_point{}) return 0;
    return static_cast<int>(std::chrono::duration_cast<std::chrono::milliseconds>(now - t).count());
}
