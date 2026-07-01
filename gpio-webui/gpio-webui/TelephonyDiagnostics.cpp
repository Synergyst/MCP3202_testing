#include "TelephonyDiagnostics.hpp"
#include "libs/audio_filters/AudioFilters.hpp"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <numeric>

using json = nlohmann::json;

namespace {
double percentile(std::vector<double> v, double p) {
    if (v.empty()) return 0.0;
    std::sort(v.begin(), v.end());
    const double pos = std::max(0.0, std::min(1.0, p)) * static_cast<double>(v.size() - 1);
    const size_t lo = static_cast<size_t>(std::floor(pos));
    const size_t hi = static_cast<size_t>(std::ceil(pos));
    const double frac = pos - static_cast<double>(lo);
    return v[lo] * (1.0 - frac) + v[hi] * frac;
}

std::string lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c){ return static_cast<char>(std::tolower(c)); });
    return s;
}
}

TelephonyDiagnostics::TelephonyDiagnostics(std::shared_ptr<SystemContext> context,
                                           AdcSampler* adc_sampler,
                                           Ch1817Driver* ch1817_driver,
                                           LineStateDetector* line_state_detector,
                                           TelephonyCoordinator* telephony_coordinator,
                                           FilterProfileManager* filter_profiles)
    : context_(std::move(context)), adc_sampler_(adc_sampler), ch1817_driver_(ch1817_driver), line_state_detector_(line_state_detector), telephony_coordinator_(telephony_coordinator), filter_profiles_(filter_profiles) {
    loadCalibration();
}

TelephonyCalibrationSettings TelephonyDiagnostics::calibrationSettings() const { return calibration_; }
json TelephonyDiagnostics::calibrationSettingsJson() const { return calibrationSettingsToJson(calibration_); }

void TelephonyDiagnostics::updateCalibrationSettingsFromJson(const json& j) {
    calibration_ = calibrationSettingsFromJson(j, calibration_);
    saveCalibration();
}

json TelephonyDiagnostics::calibrationSettingsToJson(const TelephonyCalibrationSettings& s) {
    return {{"rcv_noise_floor_rms", s.rcv_noise_floor_rms}, {"rcv_idle_peak", s.rcv_idle_peak},
            {"recommended_silence_rms", s.recommended_silence_rms}, {"recommended_min_rms", s.recommended_min_rms},
            {"tone_threshold_scale", s.tone_threshold_scale}, {"ri_mode", s.ri_mode},
            {"ri_active_low", s.ri_active_low}, {"ri_envelope_timeout_ms", s.ri_envelope_timeout_ms}};
}

TelephonyCalibrationSettings TelephonyDiagnostics::calibrationSettingsFromJson(const json& j, const TelephonyCalibrationSettings& defaults) {
    auto s = defaults;
    if (!j.is_object()) return s;
    auto clampD = [](double v, double lo, double hi){ return std::max(lo, std::min(hi, v)); };
    auto clampI = [](int v, int lo, int hi){ return std::max(lo, std::min(hi, v)); };
    s.rcv_noise_floor_rms = clampD(j.value("rcv_noise_floor_rms", s.rcv_noise_floor_rms), 0.0, 1.0);
    s.rcv_idle_peak = clampD(j.value("rcv_idle_peak", s.rcv_idle_peak), 0.0, 1.0);
    s.recommended_silence_rms = clampD(j.value("recommended_silence_rms", s.recommended_silence_rms), 0.0, 1.0);
    s.recommended_min_rms = clampD(j.value("recommended_min_rms", s.recommended_min_rms), 0.0, 1.0);
    s.tone_threshold_scale = clampD(j.value("tone_threshold_scale", s.tone_threshold_scale), 0.05, 10.0);
    s.ri_mode = lower(j.value("ri_mode", s.ri_mode));
    if (s.ri_mode != "auto" && s.ri_mode != "pulsed" && s.ri_mode != "envelope") s.ri_mode = "auto";
    s.ri_active_low = j.value("ri_active_low", s.ri_active_low);
    s.ri_envelope_timeout_ms = clampI(j.value("ri_envelope_timeout_ms", s.ri_envelope_timeout_ms), 100, 10000);
    return s;
}

std::vector<float> TelephonyDiagnostics::latestWindowMs(size_t duration_ms, uint32_t& sample_rate) const {
    sample_rate = 0;
    if (!adc_sampler_ || !context_ || !context_->signal_buffer) return {};
    auto adc = adc_sampler_->status();
    sample_rate = adc.measured_sample_rate_hz ? adc.measured_sample_rate_hz : adc.sample_rate_hz;
    if (sample_rate == 0) return {};
    const size_t count = std::max<size_t>(1, (static_cast<uint64_t>(sample_rate) * duration_ms + 999) / 1000);
    return context_->signal_buffer->getLatestWindow(count);
}

json TelephonyDiagnostics::summarizeSamples(const std::vector<float>& samples, uint32_t sample_rate_hz) {
    auto s = dsp::computeStats(samples, sample_rate_hz);
    return {{"samples", s.samples}, {"sample_rate_hz", sample_rate_hz}, {"mean", s.mean}, {"rms", s.rms},
            {"peak", s.peak_abs}, {"min", s.min}, {"max", s.max}, {"energy", s.energy}, {"zero_crossing_hz", s.zero_crossing_rate_hz}};
}

json TelephonyDiagnostics::recommendThresholds(const std::vector<double>& rms_values, const std::vector<double>& peak_values) {
    const double rms_mean = rms_values.empty() ? 0.0 : std::accumulate(rms_values.begin(), rms_values.end(), 0.0) / static_cast<double>(rms_values.size());
    const double rms_p95 = percentile(rms_values, 0.95);
    const double peak_p95 = percentile(peak_values, 0.95);
    const double silence = std::min(1.0, std::max(0.00005, rms_p95 * 1.5));
    const double min_rms = std::min(1.0, std::max(silence * 2.0, rms_p95 * 4.0));
    return {{"rms_mean", rms_mean}, {"rms_p95", rms_p95}, {"peak_p95", peak_p95},
            {"recommended_silence_rms", silence}, {"recommended_min_rms", min_rms}};
}

json TelephonyDiagnostics::calibrateRcvNoiseFloor(size_t duration_ms, size_t window_ms) const {
    uint32_t sr = 0;
    auto samples = latestWindowMs(std::max<size_t>(100, duration_ms), sr);
    if (samples.empty() || sr == 0) return {{"status", "error"}, {"error", "no RCV samples available"}};
    std::vector<std::string> filter_effects;
    if (filter_profiles_) { filter_effects = filter_profiles_->effectiveEffects("telephony.diagnostics"); if (!filter_effects.empty()) AudioFilters::applyEffectsToFloatMono(samples, sr, filter_effects); }
    const size_t win = std::max<size_t>(8, (static_cast<uint64_t>(sr) * std::max<size_t>(20, window_ms) + 999) / 1000);
    std::vector<double> rms_values, peak_values;
    for (size_t pos = 0; pos < samples.size(); pos += win) {
        const size_t end = std::min(samples.size(), pos + win);
        std::vector<float> chunk(samples.begin() + static_cast<std::ptrdiff_t>(pos), samples.begin() + static_cast<std::ptrdiff_t>(end));
        auto st = dsp::computeStats(chunk, sr);
        rms_values.push_back(st.rms);
        peak_values.push_back(st.peak_abs);
    }
    auto rec = recommendThresholds(rms_values, peak_values);
    return {{"status", "ok"}, {"duration_ms", duration_ms}, {"window_ms", window_ms}, {"sample_rate_hz", sr},
            {"filter_profile", "telephony.diagnostics"}, {"filter_effects", filter_effects},
            {"samples", samples.size()}, {"windows", rms_values.size()}, {"summary", summarizeSamples(samples, sr)}, {"recommendations", rec}};
}

json TelephonyDiagnostics::toneScan(size_t duration_ms, const std::string& region) const {
    uint32_t sr = 0;
    auto samples = latestWindowMs(std::max<size_t>(50, duration_ms), sr);
    if (samples.empty() || sr == 0) return {{"status", "error"}, {"error", "no RCV samples available"}};
    std::vector<std::string> filter_effects;
    if (filter_profiles_) { filter_effects = filter_profiles_->effectiveEffects("telephony.diagnostics"); if (!filter_effects.empty()) AudioFilters::applyEffectsToFloatMono(samples, sr, filter_effects); }
    auto profile = LineStateDetector::builtInProfile(region);
    std::vector<double> freqs;
    for (const auto& t : profile.tones) for (double f : t.frequencies_hz) freqs.push_back(f);
    std::sort(freqs.begin(), freqs.end());
    freqs.erase(std::unique(freqs.begin(), freqs.end()), freqs.end());
    auto bins = dsp::goertzelMany(samples, sr, freqs, true, dsp::WindowFunction::Hann);
    json tones = json::array();
    for (const auto& b : bins) tones.push_back({{"frequency_hz", b.frequency_hz}, {"power", b.power}, {"normalized_power", b.normalized_power}, {"relative_db", b.relative_db}});
    auto settings = line_state_detector_ ? line_state_detector_->settings() : LineStateDetector::Settings();
    auto analysis = LineStateDetector::analyzeSamplesForTest(samples, sr, settings, profile, false);
    return {{"status", "ok"}, {"duration_ms", duration_ms}, {"region", profile.id}, {"region_label", profile.label},
            {"filter_profile", "telephony.diagnostics"}, {"filter_effects", filter_effects},
            {"summary", summarizeSamples(samples, sr)}, {"detected_state", LineStateDetector::stateText(analysis.state)},
            {"confidence", analysis.confidence}, {"best_tone", analysis.tone_name}, {"tones", tones},
            {"recommended_tone_detect_threshold", std::max(0.02, std::min(0.20, analysis.confidence * 0.35))}};
}

json TelephonyDiagnostics::captureDisconnectProfile(size_t duration_ms, const std::string& label) const {
    uint32_t sr = 0;
    auto samples = latestWindowMs(std::max<size_t>(100, duration_ms), sr);
    if (samples.empty() || sr == 0) return {{"status", "error"}, {"error", "no RCV samples available"}};
    std::vector<std::string> filter_effects;
    if (filter_profiles_) { filter_effects = filter_profiles_->effectiveEffects("telephony.diagnostics"); if (!filter_effects.empty()) AudioFilters::applyEffectsToFloatMono(samples, sr, filter_effects); }

    auto spectrum = dsp::fftMagnitudeSpectrum(samples, sr, true, dsp::WindowFunction::Hann, true);
    std::sort(spectrum.begin(), spectrum.end(), [](const auto& a, const auto& b){ return a.power > b.power; });
    json dominant = json::array();
    std::vector<double> dominant_freqs;
    for (const auto& b : spectrum) {
        if (b.frequency_hz < 80.0 || b.frequency_hz > 3600.0) continue;
        bool too_close = false;
        for (double f : dominant_freqs) if (std::abs(f - b.frequency_hz) < 40.0) too_close = true;
        if (too_close) continue;
        dominant_freqs.push_back(b.frequency_hz);
        dominant.push_back({{"frequency_hz", b.frequency_hz}, {"power", b.power}, {"magnitude", b.magnitude}});
        if (dominant_freqs.size() >= 8) break;
    }

    // Estimate cadence from short-window RMS activity. This is a helper/skeleton for saving ATA profiles.
    const size_t win = std::max<size_t>(8, (static_cast<uint64_t>(sr) * 50 + 999) / 1000);
    std::vector<double> rms_values;
    for (size_t pos = 0; pos < samples.size(); pos += win) {
        const size_t end = std::min(samples.size(), pos + win);
        std::vector<float> chunk(samples.begin() + static_cast<std::ptrdiff_t>(pos), samples.begin() + static_cast<std::ptrdiff_t>(end));
        rms_values.push_back(dsp::computeStats(chunk, sr).rms);
    }
    const double p50 = percentile(rms_values, 0.50);
    const double p90 = percentile(rms_values, 0.90);
    const double threshold = std::max(0.00005, (p50 + p90) * 0.5);
    json cadence = json::array();
    bool initialized = false, active = false;
    int dur_ms = 0;
    for (double r : rms_values) {
        bool a = r >= threshold;
        if (!initialized) { initialized = true; active = a; dur_ms = 50; continue; }
        if (a == active) dur_ms += 50;
        else { cadence.push_back({{"active", active}, {"duration_ms", dur_ms}}); active = a; dur_ms = 50; }
    }
    if (initialized) cadence.push_back({{"active", active}, {"duration_ms", dur_ms}});

    json suggested = {
        {"label", label},
        {"frequencies_hz", dominant_freqs},
        {"cadence_segments", cadence},
        {"notes", "Review dominant frequencies/cadence, then convert into a disconnect_detection profile."}
    };
    return {{"status", "ok"}, {"duration_ms", duration_ms}, {"sample_rate_hz", sr},
            {"filter_profile", "telephony.diagnostics"}, {"filter_effects", filter_effects},
            {"summary", summarizeSamples(samples, sr)}, {"dominant_tones", dominant},
            {"activity_threshold_rms", threshold}, {"suggested_profile", suggested}};
}

std::vector<std::string> TelephonyDiagnostics::buildWarnings(const json& adc, const json& ch, const json& line, const json& coord, const TelephonyCalibrationSettings& cal) {
    std::vector<std::string> w;
    if (!adc.value("healthy", false)) w.push_back("ADC sampler is not healthy");
    const auto latest = adc.value("latest_raw", std::vector<uint16_t>{});
    if (!latest.empty() && (latest[0] < 10 || latest[0] > 4085)) w.push_back("RCV/CH0 raw ADC is near rail; possible clipping or bias issue");
    if (!ch.value("enabled", false)) w.push_back("CH1817 driver is unavailable");
    if (ch.value("ri_level", true) == false && !line.value("ri_ringing", false)) w.push_back("RI is LOW; answer requests must remain blocked until RI returns HIGH");
    if (line.value("rms", 0.0) > 0.0 && line.value("rms", 0.0) < cal.rcv_noise_floor_rms * 0.8 && line.value("state", std::string("")) != "silence") w.push_back("Line-state reports a signal near/below calibrated noise floor");
    if (line.value("peak", 0.0) > 0.98) w.push_back("RCV normalized peak is near clipping");
    if (coord.value("settings", json::object()).value("auto_answer_enabled", false) && !ch.value("enabled", false)) w.push_back("Auto-answer is enabled but CH1817 is unavailable");
    if (coord.value("settings", json::object()).value("caller_id_before_auto_answer", false) && !coord.value("inputs", json::object()).value("caller_available", false)) w.push_back("Caller-ID-before-answer is enabled but Caller ID detector is unavailable");
    if (cal.ri_mode == "pulsed") {
        const double f = ch.value("ri_frequency_hz", 0.0);
        if (ch.value("ringing", false) && (f < 15.0 || f > 68.0)) w.push_back("RI pulsed mode selected but measured RI frequency is outside 15..68 Hz expected range");
    }
    return w;
}

json TelephonyDiagnostics::hardwareCheck() const {
    json adc = json{{"healthy", false}, {"enabled", false}};
    if (adc_sampler_) {
        auto s = adc_sampler_->status();
        adc = {{"enabled", s.enabled}, {"running", s.running}, {"healthy", s.healthy},
               {"adc_source", s.adc_source}, {"sample_rate_hz", s.sample_rate_hz},
               {"measured_sample_rate_hz", s.measured_sample_rate_hz},
               {"latest_raw", {s.latest_raw[0], s.latest_raw[1]}},
               {"latest_volts", {s.latest_volts[0], s.latest_volts[1]}},
               {"total_frames", s.total_frames}, {"dropped_reads", s.dropped_reads},
               {"last_error", s.last_error}};
    }
    json ch = ch1817_driver_ ? ch1817_driver_->snapshotJson() : json{{"enabled", false}};
    json line = line_state_detector_ ? line_state_detector_->snapshotJson() : json{{"enabled", false}};
    json coord = telephony_coordinator_ ? telephony_coordinator_->snapshotJson() : json{{"enabled", false}};
    auto warnings = buildWarnings(adc, ch, line, coord, calibration_);
    json jw = json::array(); for (const auto& s : warnings) jw.push_back(s);
    return {{"status", warnings.empty() ? "ok" : "warning"}, {"calibration", calibrationSettingsToJson(calibration_)},
            {"adc", adc}, {"ch1817", ch}, {"line_state", line}, {"telephony", coord}, {"warnings", jw}};
}

json TelephonyDiagnostics::diagnosticsExport() const {
    return {{"hardware_check", hardwareCheck()}, {"events", eventsJson()}, {"validation_checklist", validationChecklist()}};
}

json TelephonyDiagnostics::eventsJson() const {
    json coord = telephony_coordinator_ ? telephony_coordinator_->snapshotJson() : json{{"events", json::array()}};
    return {{"events", coord.value("events", json::array())}};
}

json TelephonyDiagnostics::validationChecklist() const {
    return {{"items", json::array({
        "Confirm CH1817 VCC is +5V +/-5% and GND noise is low.",
        "Confirm RI is HIGH while idle/on-hook.",
        "Confirm RCV is AC-coupled into CH0 and biased for the ADC input range.",
        "Run RCV noise-floor calibration while the line is idle.",
        "Place an incoming call and confirm RI goes LOW or pulses during the ring burst.",
        "Confirm line-state changes to ringing and coordinator ring count increments.",
        "Confirm Caller ID is received before auto-answer if that policy is enabled.",
        "Confirm OFFHK is not asserted while RI is LOW.",
        "Confirm OFFHK can be asserted during RI HIGH safe gap.",
        "After off-hook, confirm dial tone/busy/reorder/ringback classifications as appropriate."
    })}};
}

void TelephonyDiagnostics::loadCalibration() {
    if (!context_) return;
    std::lock_guard<std::mutex> lock(context_->config_mutex);
    std::ifstream f(context_->config_path);
    if (!f.is_open()) return;
    try { json j; f >> j; if (j.contains("hardware_calibration")) calibration_ = calibrationSettingsFromJson(j["hardware_calibration"], calibration_); } catch (...) {}
}

void TelephonyDiagnostics::saveCalibration() const {
    if (!context_) return;
    std::lock_guard<std::mutex> lock(context_->config_mutex);
    json j; { std::ifstream f(context_->config_path); if (f.is_open()) { try { f >> j; } catch (...) { j = json::object(); } } }
    if (!j.is_object()) j = json::object();
    j["hardware_calibration"] = calibrationSettingsToJson(calibration_);
    std::ofstream out(context_->config_path); if (out.is_open()) out << j.dump(2);
}
