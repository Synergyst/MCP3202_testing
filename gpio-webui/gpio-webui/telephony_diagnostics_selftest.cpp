#include "TelephonyDiagnostics.hpp"

#include <iostream>
#include <stdexcept>
#include <vector>

namespace {
void require(bool ok, const std::string& msg) { if (!ok) throw std::runtime_error(msg); }
}

int main() {
    try {
        TelephonyCalibrationSettings defaults;
        nlohmann::json j = {{"ri_mode", "pulsed"}, {"rcv_noise_floor_rms", 0.001}, {"ri_envelope_timeout_ms", 900}};
        auto s = TelephonyDiagnostics::calibrationSettingsFromJson(j, defaults);
        require(s.ri_mode == "pulsed", "ri_mode parse failed");
        require(s.rcv_noise_floor_rms == 0.001, "noise floor parse failed");

        std::vector<float> samples(800, 0.001f);
        auto summary = TelephonyDiagnostics::summarizeSamples(samples, 8000);
        require(summary.value("samples", 0) == 800, "summary sample count failed");

        std::vector<double> rms = {0.0004, 0.0005, 0.0006, 0.0007, 0.0010};
        std::vector<double> peak = {0.001, 0.0012, 0.0015, 0.002, 0.003};
        auto rec = TelephonyDiagnostics::recommendThresholds(rms, peak);
        require(rec.value("recommended_silence_rms", 0.0) > rec.value("rms_p95", 0.0), "silence threshold should exceed noise p95");
        require(rec.value("recommended_min_rms", 0.0) > rec.value("recommended_silence_rms", 0.0), "min rms should exceed silence threshold");

        nlohmann::json adc = {{"healthy", false}, {"latest_raw", {0, 2000}}};
        nlohmann::json ch = {{"enabled", true}, {"ri_level", false}, {"ringing", true}, {"ri_frequency_hz", 80.0}};
        nlohmann::json line = {{"state", "silence"}, {"rms", 0.0001}, {"peak", 0.99}, {"ri_ringing", false}};
        nlohmann::json coord = {{"settings", {{"auto_answer_enabled", true}, {"caller_id_before_auto_answer", true}}}, {"inputs", {{"caller_available", false}}}};
        s.ri_mode = "pulsed";
        auto warnings = TelephonyDiagnostics::buildWarnings(adc, ch, line, coord, s);
        std::cout << "warnings=" << warnings.size() << "\n";
        require(warnings.size() >= 4, "expected multiple diagnostics warnings");

        TelephonyDiagnostics diag(nullptr, nullptr, nullptr, nullptr, nullptr);
        auto profile = diag.captureDisconnectProfile(500, "no_samples");
        require(profile.value("status", "") == "error", "capture profile should report error without samples");

        std::cout << "Telephony diagnostics self-test: PASS\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "Telephony diagnostics self-test: FAIL: " << e.what() << "\n";
        return 1;
    }
}
