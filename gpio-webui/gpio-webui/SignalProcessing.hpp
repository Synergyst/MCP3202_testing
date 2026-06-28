#pragma once

#include "SharedSignalBuffer.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace dsp {

struct WindowStats {
    size_t samples = 0;
    double mean = 0.0;
    double rms = 0.0;
    double peak_abs = 0.0;
    double min = 0.0;
    double max = 0.0;
    double energy = 0.0;
    double zero_crossing_rate_hz = 0.0;
};

enum class WindowFunction {
    Rectangular,
    Hann,
    Hamming,
    Blackman
};

struct GoertzelResult {
    double frequency_hz = 0.0;
    double magnitude = 0.0;
    double power = 0.0;
    double normalized_power = 0.0;
    double relative_db = -120.0;
};

struct SpectrumBin {
    double frequency_hz = 0.0;
    double magnitude = 0.0;
    double power = 0.0;
};

struct ToneEnergy {
    double frequency_hz = 0.0;
    double power = 0.0;
    double normalized_power = 0.0;
    double relative_db = -120.0;
};

struct ToneDetectionResult {
    bool detected = false;
    double confidence = 0.0;
    double dominant_frequency_hz = 0.0;
    std::vector<ToneEnergy> tones;
};

struct SignalWindow {
    std::vector<float> samples;
    uint32_t sample_rate_hz = 0;
    std::chrono::steady_clock::time_point captured_at{};

    size_t size() const { return samples.size(); }
    double durationMs() const;
    bool empty() const { return samples.empty(); }
};

class SignalWindowReader {
public:
    SignalWindowReader() = default;
    SignalWindowReader(std::shared_ptr<SharedSignalBuffer> buffer, uint32_t sample_rate_hz);

    void setBuffer(std::shared_ptr<SharedSignalBuffer> buffer);
    void setSampleRate(uint32_t sample_rate_hz);
    uint32_t sampleRate() const;

    SignalWindow latest(size_t sample_count) const;
    SignalWindow latestMs(double milliseconds) const;

private:
    std::shared_ptr<SharedSignalBuffer> buffer_;
    uint32_t sample_rate_hz_ = 0;
};

WindowStats computeStats(const std::vector<float>& samples, double sample_rate_hz = 0.0);
double mean(const std::vector<float>& samples);
double rms(const std::vector<float>& samples, bool remove_dc = false);
double peakAbs(const std::vector<float>& samples);
double energy(const std::vector<float>& samples, bool remove_dc = false);

void removeMeanInPlace(std::vector<float>& samples);
std::vector<float> removeMeanCopy(const std::vector<float>& samples);
void normalizePeakInPlace(std::vector<float>& samples, float target_peak = 1.0f);

void applyWindowInPlace(std::vector<float>& samples, WindowFunction window);
std::vector<float> windowedCopy(const std::vector<float>& samples, WindowFunction window);
std::string windowFunctionName(WindowFunction window);

GoertzelResult goertzel(const std::vector<float>& samples,
                        double sample_rate_hz,
                        double target_frequency_hz,
                        bool remove_dc = true,
                        WindowFunction window = WindowFunction::Hann);

std::vector<GoertzelResult> goertzelMany(const std::vector<float>& samples,
                                         double sample_rate_hz,
                                         const std::vector<double>& target_frequencies_hz,
                                         bool remove_dc = true,
                                         WindowFunction window = WindowFunction::Hann);

ToneDetectionResult detectTones(const std::vector<float>& samples,
                                double sample_rate_hz,
                                const std::vector<double>& target_frequencies_hz,
                                double min_normalized_power = 0.12,
                                double min_relative_db = -18.0,
                                bool remove_dc = true,
                                WindowFunction window = WindowFunction::Hann);

bool isPowerOfTwo(size_t n);
size_t nextPowerOfTwo(size_t n);

std::vector<SpectrumBin> fftMagnitudeSpectrum(const std::vector<float>& samples,
                                              double sample_rate_hz,
                                              bool remove_dc = true,
                                              WindowFunction window = WindowFunction::Hann,
                                              bool zero_pad_to_power_of_two = true);

} // namespace dsp
