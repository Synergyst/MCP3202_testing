#pragma once

#include "AdcSampler.hpp"
#include "Ch1817Driver.hpp"
#include "LineStateDetector.hpp"
#include "SignalProcessing.hpp"
#include "SystemContext.hpp"
#include "TelephonyCoordinator.hpp"
#include "FilterProfileManager.hpp"

#include <memory>
#include <nlohmann/json.hpp>
#include <string>
#include <vector>

struct TelephonyCalibrationSettings {
    double rcv_noise_floor_rms = 0.0005;
    double rcv_idle_peak = 0.002;
    double recommended_silence_rms = 0.0008;
    double recommended_min_rms = 0.002;
    double tone_threshold_scale = 1.0;
    std::string ri_mode = "auto"; // auto, pulsed, envelope
    bool ri_active_low = true;
    int ri_envelope_timeout_ms = 1200;
};

class TelephonyDiagnostics {
public:
    TelephonyDiagnostics(std::shared_ptr<SystemContext> context,
                         AdcSampler* adc_sampler,
                         Ch1817Driver* ch1817_driver,
                         LineStateDetector* line_state_detector,
                         TelephonyCoordinator* telephony_coordinator,
                         FilterProfileManager* filter_profiles = nullptr);

    TelephonyCalibrationSettings calibrationSettings() const;
    nlohmann::json calibrationSettingsJson() const;
    void updateCalibrationSettingsFromJson(const nlohmann::json& j);

    nlohmann::json calibrateRcvNoiseFloor(size_t duration_ms = 3000, size_t window_ms = 100) const;
    nlohmann::json toneScan(size_t duration_ms = 1000, const std::string& region = "nanp") const;
    nlohmann::json captureDisconnectProfile(size_t duration_ms = 3000, const std::string& label = "ata_custom") const;
    nlohmann::json hardwareCheck() const;
    nlohmann::json diagnosticsExport() const;
    nlohmann::json eventsJson() const;
    nlohmann::json validationChecklist() const;

    static nlohmann::json calibrationSettingsToJson(const TelephonyCalibrationSettings& s);
    static TelephonyCalibrationSettings calibrationSettingsFromJson(const nlohmann::json& j, const TelephonyCalibrationSettings& defaults);
    static nlohmann::json summarizeSamples(const std::vector<float>& samples, uint32_t sample_rate_hz);
    static nlohmann::json recommendThresholds(const std::vector<double>& rms_values, const std::vector<double>& peak_values);
    static std::vector<std::string> buildWarnings(const nlohmann::json& adc,
                                                   const nlohmann::json& ch,
                                                   const nlohmann::json& line,
                                                   const nlohmann::json& coord,
                                                   const TelephonyCalibrationSettings& cal);

private:
    void loadCalibration();
    void saveCalibration() const;
    std::vector<float> latestWindowMs(size_t duration_ms, uint32_t& sample_rate) const;

    std::shared_ptr<SystemContext> context_;
    AdcSampler* adc_sampler_ = nullptr;
    Ch1817Driver* ch1817_driver_ = nullptr;
    LineStateDetector* line_state_detector_ = nullptr;
    TelephonyCoordinator* telephony_coordinator_ = nullptr;
    FilterProfileManager* filter_profiles_ = nullptr;
    mutable TelephonyCalibrationSettings calibration_;
};
