#include "SignalProcessing.hpp"

#include <algorithm>
#include <cmath>
#include <iostream>
#include <stdexcept>
#include <vector>

namespace {
constexpr double kPi = 3.141592653589793238462643383279502884;

std::vector<float> sine(double hz, double sr, double seconds, double amp = 0.8, double dc = 0.0) {
    const size_t n = static_cast<size_t>(std::llround(sr * seconds));
    std::vector<float> x(n);
    for (size_t i = 0; i < n; ++i) {
        x[i] = static_cast<float>(dc + amp * std::sin(2.0 * kPi * hz * static_cast<double>(i) / sr));
    }
    return x;
}

std::vector<float> dual(double hz1, double hz2, double sr, double seconds, double amp1 = 0.5, double amp2 = 0.5) {
    const size_t n = static_cast<size_t>(std::llround(sr * seconds));
    std::vector<float> x(n);
    for (size_t i = 0; i < n; ++i) {
        const double t = static_cast<double>(i) / sr;
        x[i] = static_cast<float>(amp1 * std::sin(2.0 * kPi * hz1 * t) + amp2 * std::sin(2.0 * kPi * hz2 * t));
    }
    return x;
}

void require(bool ok, const std::string& msg) {
    if (!ok) throw std::runtime_error(msg);
}
}

int main() {
    try {
        constexpr double sr = 8000.0;

        auto x1200 = sine(1200.0, sr, 0.100, 0.75, 0.15);
        auto stats = dsp::computeStats(x1200, sr);
        std::cout << "stats samples=" << stats.samples
                  << " mean=" << stats.mean
                  << " rms=" << stats.rms
                  << " peak=" << stats.peak_abs
                  << " zcr_hz=" << stats.zero_crossing_rate_hz << "\n";
        require(stats.samples == 800, "unexpected sample count");
        require(stats.rms > 0.45 && stats.rms < 0.60, "unexpected RMS for sine");

        auto g1200 = dsp::goertzel(x1200, sr, 1200.0);
        auto g2200 = dsp::goertzel(x1200, sr, 2200.0);
        std::cout << "goertzel 1200 norm=" << g1200.normalized_power << " db=" << g1200.relative_db
                  << " 2200 norm=" << g2200.normalized_power << " db=" << g2200.relative_db << "\n";
        require(g1200.power > g2200.power * 20.0, "1200 Hz tone was not dominant enough");
        require(g1200.normalized_power > 0.10, "1200 Hz normalized power too low");

        auto xdual = dual(350.0, 440.0, sr, 0.128, 0.5, 0.5);
        auto tones = dsp::detectTones(xdual, sr, {350.0, 440.0, 480.0, 620.0}, 0.04, -24.0);
        std::cout << "tones detected=" << tones.detected << " confidence=" << tones.confidence
                  << " dominant=" << tones.dominant_frequency_hz << "\n";
        require(tones.detected, "dual-tone detection failed");
        require(tones.dominant_frequency_hz == 350.0 || tones.dominant_frequency_hz == 440.0,
                "unexpected dominant frequency");

        auto spec = dsp::fftMagnitudeSpectrum(x1200, sr);
        auto best = std::max_element(spec.begin(), spec.end(), [](const auto& a, const auto& b) { return a.power < b.power; });
        require(best != spec.end(), "empty FFT spectrum");
        std::cout << "fft dominant=" << best->frequency_hz << "Hz power=" << best->power << "\n";
        require(std::abs(best->frequency_hz - 1200.0) < 40.0, "FFT dominant frequency is not near 1200 Hz");

        auto shared = std::make_shared<SharedSignalBuffer>(1024);
        for (float v : x1200) shared->push(v);
        dsp::SignalWindowReader reader(shared, static_cast<uint32_t>(sr));
        auto win = reader.latestMs(50.0);
        std::cout << "window samples=" << win.samples.size() << " duration_ms=" << win.durationMs() << "\n";
        require(win.samples.size() == 400, "SignalWindowReader latestMs returned wrong size");

        std::cout << "DSP self-test: PASS\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "DSP self-test: FAIL: " << e.what() << "\n";
        return 1;
    }
}
