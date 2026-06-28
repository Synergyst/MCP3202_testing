#include "SignalProcessing.hpp"

#include <algorithm>
#include <cmath>
#include <complex>
#include <limits>
#include <numeric>
#include <stdexcept>

namespace dsp {
namespace {
constexpr double kPi = 3.141592653589793238462643383279502884;
constexpr double kTiny = 1.0e-30;

double clamp01(double v) {
    return std::max(0.0, std::min(1.0, v));
}

double powerToDb(double p, double ref) {
    if (p <= kTiny || ref <= kTiny) return -120.0;
    return 10.0 * std::log10(p / ref);
}

void fftInPlace(std::vector<std::complex<double>>& a) {
    const size_t n = a.size();
    if (n <= 1) return;
    if (!isPowerOfTwo(n)) throw std::runtime_error("FFT length must be a power of two");

    for (size_t i = 1, j = 0; i < n; ++i) {
        size_t bit = n >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j ^= bit;
        if (i < j) std::swap(a[i], a[j]);
    }

    for (size_t len = 2; len <= n; len <<= 1) {
        const double ang = -2.0 * kPi / static_cast<double>(len);
        const std::complex<double> wlen(std::cos(ang), std::sin(ang));
        for (size_t i = 0; i < n; i += len) {
            std::complex<double> w(1.0, 0.0);
            for (size_t j = 0; j < len / 2; ++j) {
                const std::complex<double> u = a[i + j];
                const std::complex<double> v = a[i + j + len / 2] * w;
                a[i + j] = u + v;
                a[i + j + len / 2] = u - v;
                w *= wlen;
            }
        }
    }
}
} // namespace

double SignalWindow::durationMs() const {
    if (sample_rate_hz == 0) return 0.0;
    return (static_cast<double>(samples.size()) * 1000.0) / static_cast<double>(sample_rate_hz);
}

SignalWindowReader::SignalWindowReader(std::shared_ptr<SharedSignalBuffer> buffer, uint32_t sample_rate_hz)
    : buffer_(std::move(buffer)), sample_rate_hz_(sample_rate_hz) {}

void SignalWindowReader::setBuffer(std::shared_ptr<SharedSignalBuffer> buffer) { buffer_ = std::move(buffer); }
void SignalWindowReader::setSampleRate(uint32_t sample_rate_hz) { sample_rate_hz_ = sample_rate_hz; }
uint32_t SignalWindowReader::sampleRate() const { return sample_rate_hz_; }

SignalWindow SignalWindowReader::latest(size_t sample_count) const {
    SignalWindow w;
    w.sample_rate_hz = sample_rate_hz_;
    w.captured_at = std::chrono::steady_clock::now();
    if (buffer_ && sample_count > 0) w.samples = buffer_->getLatestWindow(sample_count);
    return w;
}

SignalWindow SignalWindowReader::latestMs(double milliseconds) const {
    if (sample_rate_hz_ == 0 || milliseconds <= 0.0) return latest(0);
    const auto count = static_cast<size_t>(std::ceil((static_cast<double>(sample_rate_hz_) * milliseconds) / 1000.0));
    return latest(std::max<size_t>(1, count));
}

WindowStats computeStats(const std::vector<float>& samples, double sample_rate_hz) {
    WindowStats s;
    s.samples = samples.size();
    if (samples.empty()) return s;

    s.min = std::numeric_limits<double>::infinity();
    s.max = -std::numeric_limits<double>::infinity();

    long double sum = 0.0;
    for (float fv : samples) {
        const double v = static_cast<double>(fv);
        sum += v;
        s.min = std::min(s.min, v);
        s.max = std::max(s.max, v);
        s.peak_abs = std::max(s.peak_abs, std::abs(v));
    }
    s.mean = static_cast<double>(sum / static_cast<long double>(samples.size()));

    long double e = 0.0;
    size_t crossings = 0;
    double prev = static_cast<double>(samples.front()) - s.mean;
    for (size_t i = 0; i < samples.size(); ++i) {
        const double v = static_cast<double>(samples[i]) - s.mean;
        e += v * v;
        if (i > 0) {
            if ((prev < 0.0 && v >= 0.0) || (prev >= 0.0 && v < 0.0)) crossings++;
        }
        prev = v;
    }
    s.energy = static_cast<double>(e);
    s.rms = std::sqrt(static_cast<double>(e / static_cast<long double>(samples.size())));
    if (sample_rate_hz > 0.0 && samples.size() > 1) {
        const double duration = static_cast<double>(samples.size() - 1) / sample_rate_hz;
        if (duration > 0.0) s.zero_crossing_rate_hz = static_cast<double>(crossings) / (2.0 * duration);
    }
    return s;
}

double mean(const std::vector<float>& samples) {
    if (samples.empty()) return 0.0;
    long double sum = 0.0;
    for (float v : samples) sum += v;
    return static_cast<double>(sum / static_cast<long double>(samples.size()));
}

double rms(const std::vector<float>& samples, bool remove_dc) {
    if (samples.empty()) return 0.0;
    const double m = remove_dc ? mean(samples) : 0.0;
    long double e = 0.0;
    for (float fv : samples) {
        const double v = static_cast<double>(fv) - m;
        e += v * v;
    }
    return std::sqrt(static_cast<double>(e / static_cast<long double>(samples.size())));
}

double peakAbs(const std::vector<float>& samples) {
    double p = 0.0;
    for (float v : samples) p = std::max(p, std::abs(static_cast<double>(v)));
    return p;
}

double energy(const std::vector<float>& samples, bool remove_dc) {
    const double m = remove_dc ? mean(samples) : 0.0;
    long double e = 0.0;
    for (float fv : samples) {
        const double v = static_cast<double>(fv) - m;
        e += v * v;
    }
    return static_cast<double>(e);
}

void removeMeanInPlace(std::vector<float>& samples) {
    if (samples.empty()) return;
    const float m = static_cast<float>(mean(samples));
    for (auto& v : samples) v -= m;
}

std::vector<float> removeMeanCopy(const std::vector<float>& samples) {
    auto out = samples;
    removeMeanInPlace(out);
    return out;
}

void normalizePeakInPlace(std::vector<float>& samples, float target_peak) {
    if (samples.empty() || target_peak <= 0.0f) return;
    const double p = peakAbs(samples);
    if (p <= kTiny) return;
    const float gain = static_cast<float>(static_cast<double>(target_peak) / p);
    for (auto& v : samples) v *= gain;
}

void applyWindowInPlace(std::vector<float>& samples, WindowFunction window) {
    const size_t n = samples.size();
    if (n <= 1 || window == WindowFunction::Rectangular) return;
    const double denom = static_cast<double>(n - 1);
    for (size_t i = 0; i < n; ++i) {
        const double x = static_cast<double>(i) / denom;
        double w = 1.0;
        switch (window) {
            case WindowFunction::Hann:
                w = 0.5 - 0.5 * std::cos(2.0 * kPi * x);
                break;
            case WindowFunction::Hamming:
                w = 0.54 - 0.46 * std::cos(2.0 * kPi * x);
                break;
            case WindowFunction::Blackman:
                w = 0.42 - 0.5 * std::cos(2.0 * kPi * x) + 0.08 * std::cos(4.0 * kPi * x);
                break;
            case WindowFunction::Rectangular:
                w = 1.0;
                break;
        }
        samples[i] = static_cast<float>(static_cast<double>(samples[i]) * w);
    }
}

std::vector<float> windowedCopy(const std::vector<float>& samples, WindowFunction window) {
    auto out = samples;
    applyWindowInPlace(out, window);
    return out;
}

std::string windowFunctionName(WindowFunction window) {
    switch (window) {
        case WindowFunction::Rectangular: return "rectangular";
        case WindowFunction::Hann: return "hann";
        case WindowFunction::Hamming: return "hamming";
        case WindowFunction::Blackman: return "blackman";
    }
    return "unknown";
}

GoertzelResult goertzel(const std::vector<float>& samples,
                        double sample_rate_hz,
                        double target_frequency_hz,
                        bool remove_dc,
                        WindowFunction window) {
    GoertzelResult r;
    r.frequency_hz = target_frequency_hz;
    if (samples.empty() || sample_rate_hz <= 0.0 || target_frequency_hz <= 0.0 || target_frequency_hz >= sample_rate_hz * 0.5) {
        return r;
    }

    std::vector<float> x = samples;
    if (remove_dc) removeMeanInPlace(x);
    applyWindowInPlace(x, window);

    const double total_energy = energy(x, false);
    if (total_energy <= kTiny) return r;

    // Direct-frequency Goertzel. This is not limited to integer FFT bins, which is
    // useful for telephone tones whose windows are not always bin-centered.
    const double omega = 2.0 * kPi * target_frequency_hz / sample_rate_hz;
    const double coeff = 2.0 * std::cos(omega);
    double s0 = 0.0, s1 = 0.0, s2 = 0.0;
    for (float fv : x) {
        s0 = static_cast<double>(fv) + coeff * s1 - s2;
        s2 = s1;
        s1 = s0;
    }
    const double power = s1 * s1 + s2 * s2 - coeff * s1 * s2;
    r.power = std::max(0.0, power);
    r.magnitude = std::sqrt(r.power);
    r.normalized_power = clamp01(r.power / (total_energy * static_cast<double>(x.size())));
    r.relative_db = powerToDb(r.power, total_energy * static_cast<double>(x.size()));
    return r;
}

std::vector<GoertzelResult> goertzelMany(const std::vector<float>& samples,
                                         double sample_rate_hz,
                                         const std::vector<double>& target_frequencies_hz,
                                         bool remove_dc,
                                         WindowFunction window) {
    std::vector<GoertzelResult> out;
    out.reserve(target_frequencies_hz.size());
    for (double f : target_frequencies_hz) out.push_back(goertzel(samples, sample_rate_hz, f, remove_dc, window));
    return out;
}

ToneDetectionResult detectTones(const std::vector<float>& samples,
                                double sample_rate_hz,
                                const std::vector<double>& target_frequencies_hz,
                                double min_normalized_power,
                                double min_relative_db,
                                bool remove_dc,
                                WindowFunction window) {
    ToneDetectionResult result;
    auto bins = goertzelMany(samples, sample_rate_hz, target_frequencies_hz, remove_dc, window);
    result.tones.reserve(bins.size());

    double best_power = 0.0;
    double second_power = 0.0;
    double best_norm = 0.0;
    double best_db = -120.0;
    for (const auto& b : bins) {
        result.tones.push_back({b.frequency_hz, b.power, b.normalized_power, b.relative_db});
        if (b.power > best_power) {
            second_power = best_power;
            best_power = b.power;
            best_norm = b.normalized_power;
            best_db = b.relative_db;
            result.dominant_frequency_hz = b.frequency_hz;
        } else if (b.power > second_power) {
            second_power = b.power;
        }
    }

    const double separation = best_power > kTiny ? (best_power - second_power) / best_power : 0.0;
    const double norm_score = min_normalized_power > 0.0 ? clamp01(best_norm / min_normalized_power) : best_norm;
    const double db_score = best_db >= min_relative_db ? 1.0 : clamp01((best_db + 60.0) / std::max(1.0, min_relative_db + 60.0));
    result.confidence = clamp01(0.60 * norm_score + 0.25 * separation + 0.15 * db_score);
    result.detected = best_norm >= min_normalized_power && best_db >= min_relative_db;
    return result;
}

bool isPowerOfTwo(size_t n) {
    return n != 0 && (n & (n - 1)) == 0;
}

size_t nextPowerOfTwo(size_t n) {
    if (n <= 1) return 1;
    --n;
    for (size_t shift = 1; shift < sizeof(size_t) * 8; shift <<= 1) n |= n >> shift;
    return n + 1;
}

std::vector<SpectrumBin> fftMagnitudeSpectrum(const std::vector<float>& samples,
                                              double sample_rate_hz,
                                              bool remove_dc,
                                              WindowFunction window,
                                              bool zero_pad_to_power_of_two) {
    if (samples.empty() || sample_rate_hz <= 0.0) return {};
    size_t n = samples.size();
    if (!isPowerOfTwo(n)) {
        if (!zero_pad_to_power_of_two) return {};
        n = nextPowerOfTwo(n);
    }

    std::vector<float> x = samples;
    if (remove_dc) removeMeanInPlace(x);
    applyWindowInPlace(x, window);

    std::vector<std::complex<double>> a(n, {0.0, 0.0});
    for (size_t i = 0; i < samples.size(); ++i) a[i] = {static_cast<double>(x[i]), 0.0};
    fftInPlace(a);

    const size_t bins = n / 2 + 1;
    std::vector<SpectrumBin> out;
    out.reserve(bins);
    for (size_t k = 0; k < bins; ++k) {
        const double mag = std::abs(a[k]) / static_cast<double>(n);
        out.push_back({
            (static_cast<double>(k) * sample_rate_hz) / static_cast<double>(n),
            mag,
            mag * mag
        });
    }
    return out;
}

} // namespace dsp
