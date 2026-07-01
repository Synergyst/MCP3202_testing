#include "LineStateDetector.hpp"
#include "libs/audio_filters/AudioFilters.hpp"

#include <algorithm>
#include <cmath>
#include <ctime>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>

using json = nlohmann::json;

namespace {
int clampInt(int v, int lo, int hi) { return std::max(lo, std::min(hi, v)); }
double clampDouble(double v, double lo, double hi) { return std::max(lo, std::min(hi, v)); }
double clamp01(double v) { return std::max(0.0, std::min(1.0, v)); }

std::string lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c){ return static_cast<char>(std::tolower(c)); });
    return s;
}

std::vector<double> allProfileFrequencies(const LineStateDetector::RegionProfile& p) {
    std::vector<double> out;
    for (const auto& t : p.tones) for (double f : t.frequencies_hz) out.push_back(f);
    std::sort(out.begin(), out.end());
    out.erase(std::unique(out.begin(), out.end(), [](double a, double b){ return std::abs(a - b) < 0.001; }), out.end());
    return out;
}

double toneGroupScore(const std::vector<dsp::GoertzelResult>& bins, const LineStateDetector::ToneSpec& spec, std::vector<dsp::ToneEnergy>& energies) {
    if (spec.frequencies_hz.empty()) return 0.0;
    double sum = 0.0;
    double min_present = 1.0;
    size_t count = 0;
    for (double f : spec.frequencies_hz) {
        auto it = std::find_if(bins.begin(), bins.end(), [&](const auto& b){ return std::abs(b.frequency_hz - f) < 0.01; });
        if (it == bins.end()) continue;
        energies.push_back({it->frequency_hz, it->power, it->normalized_power, it->relative_db});
        const double n = it->normalized_power;
        sum += n;
        min_present = std::min(min_present, n);
        count++;
    }
    if (count == 0) return 0.0;
    const double avg = sum / static_cast<double>(count);
    // Two-tone signals should have both components present; single-tone profiles are fine with avg only.
    const double balance = spec.frequencies_hz.size() > 1 ? clamp01(min_present / std::max(1.0e-9, avg)) : 1.0;
    return clamp01(avg * 5.0) * (0.65 + 0.35 * balance);
}
} // namespace

LineStateDetector::LineStateDetector(std::shared_ptr<SystemContext> context, AdcSampler* sampler, Ch1817Driver* ch1817_driver, FilterProfileManager* filter_profiles)
    : context_(std::move(context)), sampler_(sampler), ch1817_driver_(ch1817_driver), filter_profiles_(filter_profiles), cadence_trackers_(16) {
    state_.enabled = sampler_ != nullptr;
    loadSettings();
}

LineStateDetector::~LineStateDetector() { stop(); }

void LineStateDetector::start() {
    if (running_) return;
    Settings s;
    { std::lock_guard<std::mutex> lock(mtx_); s = settings_; state_.enabled = s.enabled && sampler_ != nullptr; state_.running = false; }
    if (!sampler_ || !s.enabled) return;
    running_ = true;
    worker_ = std::thread(&LineStateDetector::worker, this);
}

void LineStateDetector::stop() {
    running_ = false;
    if (worker_.joinable()) worker_.join();
}

LineStateDetector::Snapshot LineStateDetector::snapshot() const {
    std::lock_guard<std::mutex> lock(mtx_);
    auto s = state_;
    s.settings = settings_;
    return s;
}

json LineStateDetector::snapshotJson() const { return snapshotToJson(snapshot()); }

LineStateDetector::Settings LineStateDetector::settings() const {
    std::lock_guard<std::mutex> lock(mtx_);
    return settings_;
}

json LineStateDetector::settingsJson() const { return settingsToJson(settings()); }

void LineStateDetector::updateSettings(const Settings& settings) {
    std::lock_guard<std::mutex> lock(mtx_);
    settings_ = settings;
    settings_.region = builtInProfile(settings_.region).id;
    state_.settings = settings_;
    state_.region = settings_.region;
    state_.region_label = builtInProfile(settings_.region).label;
    state_.status = "settings updated";
    saveSettingsLocked();
}

void LineStateDetector::updateSettingsFromJson(const json& j) {
    Settings defaults;
    { std::lock_guard<std::mutex> lock(mtx_); defaults = settings_; }
    updateSettings(settingsFromJson(j, defaults));
}

std::string LineStateDetector::stateText(State s) {
    switch (s) {
        case State::Unknown: return "unknown";
        case State::Silence: return "silence";
        case State::Idle: return "idle";
        case State::Ringing: return "ringing";
        case State::Ringback: return "ringback";
        case State::DialTone: return "dial_tone";
        case State::Busy: return "busy";
        case State::Reorder: return "reorder";
        case State::RemoteHangupTone: return "remote_hangup_tone";
        case State::DisconnectTone: return "disconnect_tone";
        case State::ReceiverOffHookTone: return "receiver_offhook_tone";
        case State::HowlerTone: return "howler_tone";
        case State::UnknownTone: return "unknown_tone";
        case State::VoiceOrAudio: return "voice_or_audio";
        case State::HardwareRiActive: return "hardware_ri_active";
    }
    return "unknown";
}

LineStateDetector::State LineStateDetector::stateFromText(const std::string& s) {
    const std::string v = lower(s);
    if (v == "silence") return State::Silence;
    if (v == "idle") return State::Idle;
    if (v == "ringing") return State::Ringing;
    if (v == "ringback") return State::Ringback;
    if (v == "dial_tone" || v == "dialtone") return State::DialTone;
    if (v == "busy") return State::Busy;
    if (v == "reorder") return State::Reorder;
    if (v == "remote_hangup_tone") return State::RemoteHangupTone;
    if (v == "disconnect_tone") return State::DisconnectTone;
    if (v == "receiver_offhook_tone") return State::ReceiverOffHookTone;
    if (v == "howler_tone") return State::HowlerTone;
    if (v == "unknown_tone") return State::UnknownTone;
    if (v == "voice_or_audio" || v == "audio") return State::VoiceOrAudio;
    if (v == "hardware_ri_active") return State::HardwareRiActive;
    return State::Unknown;
}

json LineStateDetector::settingsToJson(const Settings& s) {
    return {
        {"enabled", s.enabled}, {"region", s.region}, {"analysis_window_ms", s.analysis_window_ms},
        {"cadence_history_ms", s.cadence_history_ms}, {"update_interval_ms", s.update_interval_ms},
        {"min_rms", s.min_rms}, {"silence_rms", s.silence_rms},
        {"use_ri_corroboration", s.use_ri_corroboration}, {"prefer_ri_for_ring_start", s.prefer_ri_for_ring_start},
        {"tone_detect_threshold", s.tone_detect_threshold}, {"tone_relative_db", s.tone_relative_db},
        {"required_stable_windows", s.required_stable_windows}, {"release_stable_windows", s.release_stable_windows}
    };
}

LineStateDetector::Settings LineStateDetector::settingsFromJson(const json& j, const Settings& defaults) {
    Settings s = defaults;
    if (!j.is_object()) return s;
    s.enabled = j.value("enabled", s.enabled);
    s.region = lower(j.value("region", s.region));
    s.analysis_window_ms = static_cast<size_t>(clampInt(static_cast<int>(j.value("analysis_window_ms", s.analysis_window_ms)), 20, 2000));
    s.cadence_history_ms = static_cast<size_t>(clampInt(static_cast<int>(j.value("cadence_history_ms", s.cadence_history_ms)), 1000, 30000));
    s.update_interval_ms = static_cast<size_t>(clampInt(static_cast<int>(j.value("update_interval_ms", s.update_interval_ms)), 10, 1000));
    s.min_rms = clampDouble(j.value("min_rms", s.min_rms), 0.0, 1.0);
    s.silence_rms = clampDouble(j.value("silence_rms", s.silence_rms), 0.0, 1.0);
    s.use_ri_corroboration = j.value("use_ri_corroboration", s.use_ri_corroboration);
    s.prefer_ri_for_ring_start = j.value("prefer_ri_for_ring_start", s.prefer_ri_for_ring_start);
    s.tone_detect_threshold = clampDouble(j.value("tone_detect_threshold", s.tone_detect_threshold), 0.0, 1.0);
    s.tone_relative_db = clampDouble(j.value("tone_relative_db", s.tone_relative_db), -80.0, 0.0);
    s.required_stable_windows = clampInt(j.value("required_stable_windows", s.required_stable_windows), 1, 50);
    s.release_stable_windows = clampInt(j.value("release_stable_windows", s.release_stable_windows), 1, 100);
    s.region = builtInProfile(s.region).id;
    return s;
}

json LineStateDetector::snapshotToJson(const Snapshot& s) {
    json tones = json::array();
    for (const auto& t : s.tones) tones.push_back({{"frequency_hz", t.frequency_hz}, {"power", t.power}, {"normalized_power", t.normalized_power}, {"relative_db", t.relative_db}});
    return {
        {"enabled", s.enabled}, {"running", s.running}, {"state", stateText(s.state)}, {"state_text", s.state_text},
        {"confidence", s.confidence}, {"rms", s.rms}, {"peak", s.peak}, {"mean", s.mean},
        {"zero_crossing_hz", s.zero_crossing_hz}, {"ri_available", s.ri_available}, {"ri_level", s.ri_level},
        {"ri_active", s.ri_active}, {"ri_ringing", s.ri_ringing}, {"ri_frequency_hz", s.ri_frequency_hz},
        {"best_tone", s.best_tone}, {"region", s.region}, {"region_label", s.region_label}, {"tones", tones},
        {"analyzed_windows", s.analyzed_windows}, {"sample_rate_hz", s.sample_rate_hz}, {"window_samples", s.window_samples},
        {"status", s.status}, {"last_error", s.last_error}, {"last_transition", s.last_transition},
        {"filter_profile", s.filter_profile}, {"filter_effects", s.filter_effects}, {"settings", settingsToJson(s.settings)}
    };
}

LineStateDetector::RegionProfile LineStateDetector::builtInProfile(const std::string& region) {
    const std::string r = lower(region);
    RegionProfile p;
    p.id = "nanp";
    p.label = "North America / NANP";
    p.tones = {
        {"dial tone", State::DialTone, {350.0, 440.0}, 0.55, 0.001, true},
        {"busy tone", State::Busy, {480.0, 620.0}, 0.50, 0.001, false},
        {"reorder tone", State::Reorder, {480.0, 620.0}, 0.50, 0.001, false},
        {"remote hangup tone", State::RemoteHangupTone, {480.0, 620.0}, 0.45, 0.001, false},
        {"ringback tone", State::Ringback, {440.0, 480.0}, 0.50, 0.001, false},
        {"receiver off-hook warning", State::ReceiverOffHookTone, {1400.0, 2060.0, 2450.0, 2600.0}, 0.35, 0.001, false},
        {"howler tone", State::HowlerTone, {1400.0, 2060.0, 2450.0, 2600.0}, 0.35, 0.001, true}
    };
    p.cadences = {
        {State::Busy, "busy 500/500", 350, 750, 350, 750, 0.30},
        {State::Reorder, "reorder 250/250", 120, 360, 120, 360, 0.35},
        {State::RemoteHangupTone, "disconnect cadence", 120, 1200, 120, 2000, 0.40},
        {State::Ringback, "ringback 2s/4s", 1200, 2600, 2500, 5500, 0.35},
        {State::Ringing, "RI ring 2s/4s", 1000, 3000, 2500, 5500, 0.40}
    };
    if (r != "nanp" && r != "north_america" && r != "us" && r != "usa") return p;
    return p;
}

LineStateDetector::AnalysisResult LineStateDetector::analyzeSamplesForTest(const std::vector<float>& samples, uint32_t sample_rate_hz, const Settings& settings, const RegionProfile& profile, bool ri_active) {
    LineStateDetector dummy(nullptr, nullptr, nullptr);
    dummy.settings_ = settings;
    dummy.settings_.region = profile.id;
    return dummy.analyzeWindow(samples, sample_rate_hz, ri_active);
}

LineStateDetector::AnalysisResult LineStateDetector::analyzeWindow(const std::vector<float>& samples, uint32_t sample_rate, bool ri_active) {
    AnalysisResult out;
    auto profile = builtInProfile(settings_.region);
    const auto stats = dsp::computeStats(samples, sample_rate);
    out.rms = stats.rms;
    out.peak = stats.peak_abs;
    out.mean = stats.mean;
    out.zero_crossing_hz = stats.zero_crossing_rate_hz;

    if (samples.size() < 16 || sample_rate == 0) {
        out.state = State::Unknown;
        out.status = "not enough samples";
        return out;
    }

    if (settings_.use_ri_corroboration && ri_active && settings_.prefer_ri_for_ring_start) {
        out.state = State::Ringing;
        out.confidence = 0.78;
        out.status = "RI active/LOW fast ring indication";
        return out;
    }

    if (out.rms <= settings_.silence_rms) {
        out.state = State::Silence;
        out.confidence = clamp01(1.0 - (out.rms / std::max(1.0e-9, settings_.silence_rms)));
        out.status = "below silence RMS threshold";
        return out;
    }

    std::vector<double> freqs = allProfileFrequencies(profile);
    auto bins = dsp::goertzelMany(samples, sample_rate, freqs, true, dsp::WindowFunction::Hann);
    double best_score = 0.0;
    const ToneSpec* best_spec = nullptr;
    std::vector<dsp::ToneEnergy> best_energies;
    std::vector<std::pair<const ToneSpec*, double>> active_specs;

    for (const auto& spec : profile.tones) {
        if (out.rms < spec.min_rms) continue;
        std::vector<dsp::ToneEnergy> energies;
        double score = toneGroupScore(bins, spec, energies);
        if (!energies.empty()) {
            double avg_db = 0.0;
            for (const auto& e : energies) avg_db += e.relative_db;
            avg_db /= static_cast<double>(energies.size());
            if (avg_db < settings_.tone_relative_db) score *= 0.55;
        }
        if (score >= settings_.tone_detect_threshold && score >= spec.min_confidence * 0.5) {
            active_specs.push_back({&spec, score});
            out.active_tone_states.push_back(spec.state);
            out.tonal = true;
        }
        if (score > best_score) {
            best_score = score;
            best_spec = &spec;
            best_energies = std::move(energies);
        }
    }

    out.tones = std::move(best_energies);
    out.best_frequency_score = best_score;
    if (best_spec && best_score >= settings_.tone_detect_threshold && best_score >= best_spec->min_confidence * 0.5) {
        // 480/620 is shared by busy, reorder, and disconnect-family tones.  Frequency alone is
        // intentionally not allowed to label it as busy/reorder; cadence promotion happens in worker().
        if (best_spec->state == State::Busy || best_spec->state == State::Reorder || best_spec->state == State::RemoteHangupTone) {
            out.state = State::RemoteHangupTone;
            out.tone_name = "480/620 disconnect-family tone";
            out.confidence = clamp01(best_score * 0.85);
            out.status = "480/620 tone present; awaiting cadence classification";
        } else {
            out.state = best_spec->state;
            out.tone_name = best_spec->name;
            out.confidence = clamp01(best_score);
            out.status = "tone candidate: " + best_spec->name;
        }
    } else if (out.rms >= settings_.min_rms) {
        // A strong, narrow-band signal is more likely an unprofiled ATA warning/howler than speech.
        const double z = out.zero_crossing_hz;
        if (best_score >= settings_.tone_detect_threshold * 0.45 || (z > 900.0 && z < 3200.0)) {
            out.state = State::UnknownTone;
            out.confidence = clamp01(std::max(best_score, 0.35));
            out.tonal = true;
            out.status = "tonal audio present but no configured tone profile matched";
        } else {
            out.state = State::VoiceOrAudio;
            out.confidence = clamp01(std::min(0.70, out.rms / std::max(1.0e-9, settings_.min_rms) * 0.25));
            out.status = "broadband/non-tonal audio present without recognized tone";
        }
    } else {
        out.state = State::Idle;
        out.confidence = 0.55;
        out.status = "low non-silent signal";
    }
    return out;
}

void LineStateDetector::worker() {
    while (running_) {
        try {
            Settings cfg;
            { std::lock_guard<std::mutex> lock(mtx_); cfg = settings_; }
            if (!cfg.enabled || !sampler_) break;

            auto adc = sampler_->status();
            json ri;
            bool ri_active = false;
            if (ch1817_driver_) {
                ri = ch1817_driver_->snapshotJson();
                ri_active = ri.value("enabled", false) && !ri.value("ri_level", true);
            }

            if (!adc.healthy || adc.total_frames < 100) {
                std::lock_guard<std::mutex> lock(mtx_);
                state_.enabled = true;
                state_.running = true;
                state_.status = adc.healthy ? "waiting for samples" : "waiting for healthy ADC";
                state_.last_error = adc.last_error;
                state_.settings = settings_;
            } else {
                const uint32_t sr = std::max<uint32_t>(1, adc.measured_sample_rate_hz ? adc.measured_sample_rate_hz : adc.sample_rate_hz);
                dsp::SignalWindowReader reader(context_ ? context_->signal_buffer : nullptr, sr);
                auto win = reader.latestMs(static_cast<double>(cfg.analysis_window_ms));
                std::vector<std::string> filter_effects;
                if (filter_profiles_) {
                    filter_effects = filter_profiles_->effectiveEffects("line_state.detector");
                    if (!filter_effects.empty()) AudioFilters::applyEffectsToFloatMono(win.samples, sr, filter_effects);
                }
                AnalysisResult a;
                {
                    std::lock_guard<std::mutex> lock(mtx_);
                    a = analyzeWindow(win.samples, sr, ri_active);
                    const auto now = std::chrono::steady_clock::now();
                    auto active = [&](State st) {
                        if (a.state == st) return true;
                        return std::find(a.active_tone_states.begin(), a.active_tone_states.end(), st) != a.active_tone_states.end();
                    };
                    // Track each cadence-bearing candidate independently. This is required because
                    // busy/reorder/disconnect can share 480/620 Hz and only cadence separates them.
                    for (State st : {State::Busy, State::Reorder, State::RemoteHangupTone, State::Ringback, State::Ringing}) {
                        updateCadence(st, active(st), now);
                    }
                    const double busy_cad = cadenceConfidenceLocked(State::Busy);
                    const double reorder_cad = cadenceConfidenceLocked(State::Reorder);
                    const double disconnect_cad = cadenceConfidenceLocked(State::RemoteHangupTone);
                    if (a.state == State::RemoteHangupTone) {
                        if (busy_cad > 0.20 && busy_cad >= reorder_cad && busy_cad >= disconnect_cad) {
                            a.state = State::Busy;
                            a.tone_name = "busy tone";
                            a.confidence = clamp01(a.confidence + busy_cad);
                            a.status = "480/620 tone with busy cadence";
                        } else if (reorder_cad > 0.20 && reorder_cad >= busy_cad && reorder_cad >= disconnect_cad) {
                            a.state = State::Reorder;
                            a.tone_name = "reorder tone";
                            a.confidence = clamp01(a.confidence + reorder_cad);
                            a.status = "480/620 tone with reorder cadence";
                        } else if (disconnect_cad > 0.0) {
                            a.confidence = clamp01(a.confidence + disconnect_cad);
                            a.status += "; disconnect cadence matched";
                        }
                    } else {
                        const double cad = cadenceConfidenceLocked(a.state);
                        if (cad > 0.0) {
                            a.confidence = clamp01(a.confidence + cad);
                            a.status += "; cadence matched";
                        }
                    }
                }
                publish(a, ri, sr, win.samples.size());
                {
                    std::lock_guard<std::mutex> lock(mtx_);
                    state_.filter_profile = "line_state.detector";
                    state_.filter_effects = filter_effects;
                }
            }
        } catch (const std::exception& e) {
            std::lock_guard<std::mutex> lock(mtx_);
            state_.last_error = e.what();
            state_.status = "error";
        }
        Settings cfg;
        { std::lock_guard<std::mutex> lock(mtx_); cfg = settings_; }
        const int sleep_ms = static_cast<int>(std::max<size_t>(10, cfg.update_interval_ms));
        for (int slept = 0; running_ && slept < sleep_ms; slept += 10) std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
}

void LineStateDetector::publish(const AnalysisResult& analysis, const json& ri_json, uint32_t sample_rate, size_t window_samples) {
    std::lock_guard<std::mutex> lock(mtx_);
    State target = analysis.state;
    const bool immediate = (target == State::Ringing && ri_json.value("enabled", false) && !ri_json.value("ri_level", true));
    if (target != candidate_state_) { candidate_state_ = target; candidate_count_ = 1; }
    else candidate_count_++;
    const int needed = (target == stable_state_) ? settings_.release_stable_windows : settings_.required_stable_windows;
    if (immediate || candidate_count_ >= needed) {
        if (stable_state_ != target) state_.last_transition = nowIso8601();
        stable_state_ = target;
    }

    auto profile = builtInProfile(settings_.region);
    state_.enabled = true;
    state_.running = running_;
    state_.state = stable_state_;
    state_.state_text = stateText(stable_state_);
    state_.confidence = analysis.confidence;
    state_.rms = analysis.rms;
    state_.peak = analysis.peak;
    state_.mean = analysis.mean;
    state_.zero_crossing_hz = analysis.zero_crossing_hz;
    state_.ri_available = !ri_json.is_null() && !ri_json.empty();
    state_.ri_level = ri_json.value("ri_level", true);
    state_.ri_active = state_.ri_available && !state_.ri_level;
    state_.ri_ringing = ri_json.value("ringing", false);
    state_.ri_frequency_hz = ri_json.value("ri_frequency_hz", 0.0);
    state_.best_tone = analysis.tone_name;
    state_.region = profile.id;
    state_.region_label = profile.label;
    state_.tones = analysis.tones;
    state_.analyzed_windows++;
    state_.sample_rate_hz = sample_rate;
    state_.window_samples = window_samples;
    state_.status = analysis.status;
    state_.last_error.clear();
    state_.settings = settings_;
}

void LineStateDetector::updateCadence(State tone_state, bool active, const std::chrono::steady_clock::time_point& now) {
    const size_t idx = static_cast<size_t>(tone_state);
    if (idx >= cadence_trackers_.size()) return;
    auto& trk = cadence_trackers_[idx];
    if (!trk.initialized) { trk.initialized = true; trk.active = active; trk.last_change = now; return; }
    if (trk.active == active) return;
    int dur = static_cast<int>(std::chrono::duration_cast<std::chrono::milliseconds>(now - trk.last_change).count());
    if (dur >= 10) trk.segments.push_back({trk.active, dur});
    trk.active = active;
    trk.last_change = now;
    trimCadenceLocked(trk);
}

void LineStateDetector::trimCadenceLocked(CadenceTracker& trk) const {
    int total = 0;
    for (auto it = trk.segments.rbegin(); it != trk.segments.rend(); ++it) total += it->duration_ms;
    while (total > static_cast<int>(settings_.cadence_history_ms) && !trk.segments.empty()) {
        total -= trk.segments.front().duration_ms;
        trk.segments.pop_front();
    }
}

double LineStateDetector::cadenceConfidenceLocked(State state) const {
    const auto profile = builtInProfile(settings_.region);
    auto spec_it = std::find_if(profile.cadences.begin(), profile.cadences.end(), [&](const auto& c){ return c.state == state; });
    if (spec_it == profile.cadences.end()) return 0.0;
    const size_t idx = static_cast<size_t>(state);
    if (idx >= cadence_trackers_.size()) return 0.0;
    const auto& segs = cadence_trackers_[idx].segments;
    if (segs.size() < 2) return 0.0;
    int matched = 0, pairs = 0;
    for (size_t i = 0; i + 1 < segs.size(); ++i) {
        if (!segs[i].active || segs[i + 1].active) continue;
        pairs++;
        const int on = segs[i].duration_ms;
        const int off = segs[i + 1].duration_ms;
        if (on >= spec_it->on_min_ms && on <= spec_it->on_max_ms && off >= spec_it->off_min_ms && off <= spec_it->off_max_ms) matched++;
    }
    if (pairs == 0) return 0.0;
    return spec_it->cadence_bonus * (static_cast<double>(matched) / static_cast<double>(pairs));
}

void LineStateDetector::loadSettings() {
    if (!context_) return;
    std::lock_guard<std::mutex> cfg_lock(context_->config_mutex);
    std::ifstream f(context_->config_path);
    if (!f.is_open()) return;
    try {
        json j; f >> j;
        if (j.contains("line_state")) settings_ = settingsFromJson(j["line_state"], settings_);
        state_.settings = settings_;
        auto p = builtInProfile(settings_.region);
        state_.region = p.id;
        state_.region_label = p.label;
    } catch (const std::exception& e) {
        std::cerr << "[LineState] Config load warning: " << e.what() << std::endl;
    }
}

void LineStateDetector::saveSettingsLocked() {
    if (!context_) return;
    std::lock_guard<std::mutex> cfg_lock(context_->config_mutex);
    json j;
    { std::ifstream f(context_->config_path); if (f.is_open()) { try { f >> j; } catch (...) { j = json::object(); } } }
    if (!j.is_object()) j = json::object();
    j["line_state"] = settingsToJson(settings_);
    std::ofstream out(context_->config_path);
    if (out.is_open()) out << j.dump(2);
}

std::string LineStateDetector::nowIso8601() {
    auto now = std::chrono::system_clock::now();
    std::time_t t = std::chrono::system_clock::to_time_t(now);
    std::tm tm{};
    localtime_r(&t, &tm);
    std::ostringstream oss;
    oss << std::put_time(&tm, "%Y-%m-%dT%H:%M:%S%z");
    return oss.str();
}
