#include "AudioFilters.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>

#ifdef HAVE_RNNOISE
#include <rnnoise.h>
#endif

namespace {

int16_t clamp16(int32_t v) {
    return static_cast<int16_t>(std::max<int32_t>(-32768, std::min<int32_t>(32767, v)));
}

uint16_t clamp12(int32_t v) {
    return static_cast<uint16_t>(std::max<int32_t>(0, std::min<int32_t>(4095, v)));
}

std::string lowerCopy(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

class DcBlockFilter final : public AudioFilters::AudioFilter {
public:
    const char* id() const override { return "dc_block"; }
    void process(AudioBuffer& b) override {
        if (b.samples.empty() || b.channels == 0) return;
        const uint16_t chs = b.channels;
        std::vector<double> mean(chs, 0.0);
        std::vector<size_t> count(chs, 0);
        for (size_t i = 0; i < b.samples.size(); ++i) {
            const size_t ch = i % chs;
            mean[ch] += b.samples[i];
            count[ch]++;
        }
        for (size_t ch = 0; ch < chs; ++ch) {
            if (count[ch]) mean[ch] /= static_cast<double>(count[ch]);
        }
        for (size_t i = 0; i < b.samples.size(); ++i) {
            b.samples[i] = clamp16(static_cast<int32_t>(std::lrint(static_cast<double>(b.samples[i]) - mean[i % chs])));
        }
    }
};

class NormalizeFilter final : public AudioFilters::AudioFilter {
public:
    const char* id() const override { return "normalize"; }
    void process(AudioBuffer& b) override {
        int32_t peak = 0;
        for (int16_t s : b.samples) peak = std::max<int32_t>(peak, std::abs(static_cast<int32_t>(s)));
        if (peak <= 0) return;
        const double target = 30000.0;
        const double gain = target / static_cast<double>(peak);
        if (gain <= 1.01) return;
        for (auto& s : b.samples) s = clamp16(static_cast<int32_t>(std::lrint(static_cast<double>(s) * gain)));
    }
};

class SoftClipFilter final : public AudioFilters::AudioFilter {
public:
    const char* id() const override { return "soft_clip"; }
    void process(AudioBuffer& b) override {
        constexpr double drive = 1.6;
        const double denom = std::tanh(drive);
        for (auto& s : b.samples) {
            const double x = static_cast<double>(s) / 32768.0;
            const double y = std::tanh(x * drive) / denom;
            s = clamp16(static_cast<int32_t>(std::lrint(y * 32767.0)));
        }
    }
};

class FirstOrderBandpassFilter final : public AudioFilters::AudioFilter {
public:
    FirstOrderBandpassFilter(std::string filter_id, double hp_cut, double lp_cut, int passes)
        : id_(std::move(filter_id)), hp_cut_(hp_cut), lp_cut_requested_(lp_cut), passes_(passes) {}
    const char* id() const override { return id_.c_str(); }
    void process(AudioBuffer& b) override {
        if (b.samples.empty() || b.channels == 0 || b.sample_rate_hz == 0) return;
        const double fs = static_cast<double>(b.sample_rate_hz);
        const double lp_cut = std::min(lp_cut_requested_, fs * 0.45);
        if (hp_cut_ <= 0.0 || lp_cut <= hp_cut_) return;
        const double dt = 1.0 / fs;
        const uint16_t chs = b.channels;
        for (int pass = 0; pass < std::max(1, passes_); ++pass) {
            const double hp_rc = 1.0 / (2.0 * M_PI * hp_cut_);
            const double hp_alpha = hp_rc / (hp_rc + dt);
            const double lp_rc = 1.0 / (2.0 * M_PI * lp_cut);
            const double lp_alpha = dt / (lp_rc + dt);
            std::vector<double> hp_y(chs, 0.0), hp_prev_x(chs, 0.0), lp_y(chs, 0.0);
            for (size_t i = 0; i < b.samples.size(); ++i) {
                const size_t ch = i % chs;
                const double x = static_cast<double>(b.samples[i]);
                const double yhp = hp_alpha * (hp_y[ch] + x - hp_prev_x[ch]);
                hp_y[ch] = yhp;
                hp_prev_x[ch] = x;
                lp_y[ch] = lp_y[ch] + lp_alpha * (yhp - lp_y[ch]);
                b.samples[i] = clamp16(static_cast<int32_t>(std::lrint(lp_y[ch])));
            }
        }
    }
private:
    std::string id_;
    double hp_cut_ = 0.0;
    double lp_cut_requested_ = 0.0;
    int passes_ = 1;
};

struct Biquad {
    double b0 = 1.0, b1 = 0.0, b2 = 0.0, a1 = 0.0, a2 = 0.0;
    double z1 = 0.0, z2 = 0.0;
    double process(double x) {
        const double y = b0 * x + z1;
        z1 = b1 * x - a1 * y + z2;
        z2 = b2 * x - a2 * y;
        return y;
    }
};

Biquad makeBandpass(double sample_rate, double center_hz, double q) {
    Biquad f;
    if (sample_rate <= 0.0 || center_hz <= 0.0 || center_hz >= sample_rate * 0.45 || q <= 0.0) return f;
    const double w0 = 2.0 * M_PI * center_hz / sample_rate;
    const double alpha = std::sin(w0) / (2.0 * q);
    const double cosw0 = std::cos(w0);
    const double a0 = 1.0 + alpha;
    f.b0 = alpha / a0;
    f.b1 = 0.0;
    f.b2 = -alpha / a0;
    f.a1 = (-2.0 * cosw0) / a0;
    f.a2 = (1.0 - alpha) / a0;
    return f;
}

Biquad makeNotch(double sample_rate, double center_hz, double q) {
    Biquad f;
    if (sample_rate <= 0.0 || center_hz <= 0.0 || center_hz >= sample_rate * 0.45 || q <= 0.0) return f;
    const double w0 = 2.0 * M_PI * center_hz / sample_rate;
    const double alpha = std::sin(w0) / (2.0 * q);
    const double cosw0 = std::cos(w0);
    const double a0 = 1.0 + alpha;
    f.b0 = 1.0 / a0;
    f.b1 = (-2.0 * cosw0) / a0;
    f.b2 = 1.0 / a0;
    f.a1 = (-2.0 * cosw0) / a0;
    f.a2 = (1.0 - alpha) / a0;
    return f;
}

class HumNotchFilter final : public AudioFilters::AudioFilter {
public:
    explicit HumNotchFilter(double hz) : hz_(hz), id_(hz == 60.0 ? "hum_notch_60" : "hum_notch_120") {}
    const char* id() const override { return id_.c_str(); }
    void process(AudioBuffer& b) override {
        if (b.samples.empty() || b.channels == 0 || b.sample_rate_hz == 0) return;
        const uint16_t chs = b.channels;
        std::vector<Biquad> filters(chs);
        for (uint16_t ch = 0; ch < chs; ++ch) filters[ch] = makeNotch(static_cast<double>(b.sample_rate_hz), hz_, 35.0);
        for (size_t i = 0; i < b.samples.size(); ++i) {
            const size_t ch = i % chs;
            b.samples[i] = clamp16(static_cast<int32_t>(std::lrint(filters[ch].process(static_cast<double>(b.samples[i])))));
        }
    }
private:
    double hz_ = 60.0;
    std::string id_;
};

class Bell202DualToneFilter final : public AudioFilters::AudioFilter {
public:
    const char* id() const override { return "bell202_dual_tone"; }
    void process(AudioBuffer& b) override {
        if (b.samples.empty() || b.channels == 0 || b.sample_rate_hz == 0) return;
        const uint16_t chs = b.channels;
        const double fs = static_cast<double>(b.sample_rate_hz);
        std::vector<Biquad> mark(chs), space(chs);
        for (uint16_t ch = 0; ch < chs; ++ch) {
            mark[ch] = makeBandpass(fs, 1200.0, 8.0);
            space[ch] = makeBandpass(fs, 2200.0, 8.0);
        }
        for (size_t i = 0; i < b.samples.size(); ++i) {
            const size_t ch = i % chs;
            const double x = static_cast<double>(b.samples[i]);
            const double y = (mark[ch].process(x) + space[ch].process(x)) * 2.0;
            b.samples[i] = clamp16(static_cast<int32_t>(std::lrint(y)));
        }
    }
};

class VoiceAgcFilter final : public AudioFilters::AudioFilter {
public:
    const char* id() const override { return "voice_agc"; }
    void process(AudioBuffer& b) override {
        if (b.samples.empty() || b.channels == 0 || b.sample_rate_hz == 0) return;
        const uint16_t chs = b.channels;
        const size_t frames = b.samples.size() / chs;
        if (!frames) return;
        const double target_rms = 9000.0;
        const double max_gain = 8.0;
        const double attack = 0.10;
        const double release = 0.003;
        std::vector<double> gain(chs, 1.0);
        for (size_t i = 0; i < frames; ++i) {
            for (uint16_t ch = 0; ch < chs; ++ch) {
                double v = static_cast<double>(b.samples[i * chs + ch]);
                double desired = target_rms / std::max(1000.0, std::abs(v));
                desired = std::max(0.25, std::min(max_gain, desired));
                const double coeff = desired < gain[ch] ? attack : release;
                gain[ch] += coeff * (desired - gain[ch]);
                b.samples[i * chs + ch] = clamp16(static_cast<int32_t>(std::lrint(v * gain[ch])));
            }
        }
    }
};

class FskSquelchFilter final : public AudioFilters::AudioFilter {
public:
    const char* id() const override { return "fsk_squelch"; }
    void process(AudioBuffer& b) override {
        if (b.samples.empty() || b.channels == 0 || b.sample_rate_hz == 0) return;
        const uint16_t chs = b.channels;
        const size_t frames = b.samples.size() / chs;
        if (frames < 8) return;
        const size_t win = std::max<size_t>(8, static_cast<size_t>(b.sample_rate_hz / 200));
        for (uint16_t ch = 0; ch < chs; ++ch) {
            std::vector<double> env(frames, 0.0);
            double sumsq = 0.0;
            for (size_t i = 0; i < frames; ++i) {
                const double v = static_cast<double>(b.samples[i * chs + ch]);
                sumsq += v * v;
                if (i >= win) {
                    const double old = static_cast<double>(b.samples[(i - win) * chs + ch]);
                    sumsq -= old * old;
                }
                const size_t denom = std::min(win, i + 1);
                env[i] = std::sqrt(std::max(0.0, sumsq / static_cast<double>(denom)));
            }
            auto sorted = env;
            std::nth_element(sorted.begin(), sorted.begin() + sorted.size() / 3, sorted.end());
            const double floor = std::max(1.0, sorted[sorted.size() / 3]);
            const double open = floor * 2.2;
            const double full = floor * 5.0;
            for (size_t i = 0; i < frames; ++i) {
                double gain = 0.18;
                if (env[i] >= full) gain = 1.0;
                else if (env[i] > open) gain = 0.18 + 0.82 * ((env[i] - open) / std::max(1.0, full - open));
                const double v = static_cast<double>(b.samples[i * chs + ch]) * gain;
                b.samples[i * chs + ch] = clamp16(static_cast<int32_t>(std::lrint(v)));
            }
        }
    }
};

class RnNoiseFilter final : public AudioFilters::AudioFilter {
public:
    const char* id() const override { return "rnnoise"; }
    void process(AudioBuffer& b) override {
#ifndef HAVE_RNNOISE
        (void)b;
        throw std::runtime_error("RNNoise is not linked in this build");
#else
        if (b.samples.empty() || b.channels == 0 || b.sample_rate_hz == 0) return;
        const int rn_frame = rnnoise_get_frame_size();
        if (rn_frame <= 0) throw std::runtime_error("RNNoise reported invalid frame size");
        constexpr uint32_t rn_rate = 48000;
        const uint16_t chs = b.channels;
        const size_t input_frames = b.samples.size() / chs;
        if (input_frames == 0) return;
        const size_t rn_frames_count = std::max<size_t>(1, (static_cast<uint64_t>(input_frames) * rn_rate + b.sample_rate_hz - 1) / b.sample_rate_hz);
        std::vector<std::vector<float>> channel_in(chs, std::vector<float>(rn_frames_count, 0.0f));
        std::vector<std::vector<float>> channel_out(chs, std::vector<float>(rn_frames_count, 0.0f));
        for (uint16_t ch = 0; ch < chs; ++ch) {
            for (size_t i = 0; i < rn_frames_count; ++i) {
                const double src_pos = static_cast<double>(i) * static_cast<double>(b.sample_rate_hz) / static_cast<double>(rn_rate);
                const size_t i0 = std::min<size_t>(static_cast<size_t>(src_pos), input_frames - 1);
                const size_t i1 = std::min<size_t>(i0 + 1, input_frames - 1);
                const double frac = src_pos - static_cast<double>(i0);
                const double s0 = static_cast<double>(b.samples[i0 * chs + ch]);
                const double s1 = static_cast<double>(b.samples[i1 * chs + ch]);
                channel_in[ch][i] = static_cast<float>(s0 + (s1 - s0) * frac);
            }
        }
        const size_t padded = ((rn_frames_count + static_cast<size_t>(rn_frame) - 1) / static_cast<size_t>(rn_frame)) * static_cast<size_t>(rn_frame);
        std::vector<float> frame_in(padded, 0.0f), frame_out(padded, 0.0f);
        for (uint16_t ch = 0; ch < chs; ++ch) {
            std::unique_ptr<DenoiseState, decltype(&rnnoise_destroy)> st(rnnoise_create(nullptr), rnnoise_destroy);
            if (!st) throw std::runtime_error("rnnoise_create failed");
            std::fill(frame_in.begin(), frame_in.end(), 0.0f);
            std::copy(channel_in[ch].begin(), channel_in[ch].end(), frame_in.begin());
            for (size_t offset = 0; offset < padded; offset += static_cast<size_t>(rn_frame)) {
                rnnoise_process_frame(st.get(), frame_out.data() + offset, frame_in.data() + offset);
            }
            std::copy(frame_out.begin(), frame_out.begin() + static_cast<std::ptrdiff_t>(rn_frames_count), channel_out[ch].begin());
        }
        for (size_t i = 0; i < input_frames; ++i) {
            const double src_pos = static_cast<double>(i) * static_cast<double>(rn_rate) / static_cast<double>(b.sample_rate_hz);
            const size_t i0 = std::min<size_t>(static_cast<size_t>(src_pos), rn_frames_count - 1);
            const size_t i1 = std::min<size_t>(i0 + 1, rn_frames_count - 1);
            const double frac = src_pos - static_cast<double>(i0);
            for (uint16_t ch = 0; ch < chs; ++ch) {
                const double s0 = static_cast<double>(channel_out[ch][i0]);
                const double s1 = static_cast<double>(channel_out[ch][i1]);
                b.samples[i * chs + ch] = clamp16(static_cast<int32_t>(std::lrint(s0 + (s1 - s0) * frac)));
            }
        }
#endif
    }
};

} // namespace

namespace AudioFilters {

FilterChain::FilterChain(const std::vector<std::string>& effect_ids) { reset(effect_ids); }

void FilterChain::add(std::unique_ptr<AudioFilter> filter) {
    if (filter) filters_.push_back(std::move(filter));
}

void FilterChain::addById(const std::string& effect_id) { add(createFilter(effect_id)); }

void FilterChain::clear() { filters_.clear(); }

bool FilterChain::empty() const { return filters_.empty(); }

void FilterChain::reset(const std::vector<std::string>& effect_ids) {
    clear();
    for (const auto& effect_id : effect_ids) addById(effect_id);
}

void FilterChain::process(AudioBuffer& buffer) {
    for (auto& filter : filters_) filter->process(buffer);
}

bool isKnownEffect(const std::string& effect_id) {
    const std::string id = lowerCopy(effect_id);
    return id == "dc_block" || id == "pots_bandpass" || id == "hum_notch_60" || id == "hum_notch_120" ||
           id == "voice_agc" || id == "bell202_bandpass" || id == "bell202_dual_tone" || id == "fsk_squelch" ||
           id == "normalize" || id == "soft_clip" || id == "rnnoise";
}

bool isRuntimeAvailable(const std::string& effect_id) {
    const std::string id = lowerCopy(effect_id);
#ifndef HAVE_RNNOISE
    if (id == "rnnoise") return false;
#endif
    return isKnownEffect(id);
}

std::string unavailableReason(const std::string& effect_id) {
    const std::string id = lowerCopy(effect_id);
    if (!isKnownEffect(id)) return "Unknown audio effect: " + id;
#ifndef HAVE_RNNOISE
    if (id == "rnnoise") return "RNNoise is not linked in this build";
#endif
    return "";
}

std::unique_ptr<AudioFilter> createFilter(const std::string& effect_id) {
    const std::string id = lowerCopy(effect_id);
    if (id == "dc_block") return std::make_unique<DcBlockFilter>();
    if (id == "pots_bandpass") return std::make_unique<FirstOrderBandpassFilter>(id, 300.0, 3400.0, 1);
    if (id == "bell202_bandpass") return std::make_unique<FirstOrderBandpassFilter>(id, 700.0, 2700.0, 2);
    if (id == "hum_notch_60") return std::make_unique<HumNotchFilter>(60.0);
    if (id == "hum_notch_120") return std::make_unique<HumNotchFilter>(120.0);
    if (id == "voice_agc") return std::make_unique<VoiceAgcFilter>();
    if (id == "bell202_dual_tone") return std::make_unique<Bell202DualToneFilter>();
    if (id == "fsk_squelch") return std::make_unique<FskSquelchFilter>();
    if (id == "normalize") return std::make_unique<NormalizeFilter>();
    if (id == "soft_clip") return std::make_unique<SoftClipFilter>();
    if (id == "rnnoise") return std::make_unique<RnNoiseFilter>();
    throw std::runtime_error("Unknown or unavailable audio effect: " + id);
}

void applyEffects(AudioBuffer& buffer, const std::vector<std::string>& effect_ids) {
    FilterChain chain(effect_ids);
    chain.process(buffer);
}

int16_t rawAdcToPcm16(uint16_t raw) {
    const int32_t centered = static_cast<int32_t>(std::max<uint16_t>(0, std::min<uint16_t>(4095, raw))) - 2048;
    return clamp16(centered * 16);
}

uint16_t pcm16ToRawAdc(int16_t sample) {
    const int32_t raw = 2048 + static_cast<int32_t>(std::lrint(static_cast<double>(sample) / 16.0));
    return clamp12(raw);
}

AudioBuffer rawAdcToMonoPcm16(const std::vector<uint16_t>& raw, uint32_t sample_rate_hz) {
    AudioBuffer b;
    b.sample_rate_hz = std::max<uint32_t>(1, sample_rate_hz);
    b.channels = 1;
    b.samples.reserve(raw.size());
    for (uint16_t s : raw) b.samples.push_back(rawAdcToPcm16(s));
    return b;
}

AudioBuffer rawAdcToStereoPcm16(const std::vector<uint16_t>& ch0, const std::vector<uint16_t>& ch1, uint32_t sample_rate_hz) {
    AudioBuffer b;
    b.sample_rate_hz = std::max<uint32_t>(1, sample_rate_hz);
    b.channels = 2;
    const size_t frames = std::min(ch0.size(), ch1.size());
    b.samples.reserve(frames * 2);
    for (size_t i = 0; i < frames; ++i) {
        b.samples.push_back(rawAdcToPcm16(ch0[i]));
        b.samples.push_back(rawAdcToPcm16(ch1[i]));
    }
    return b;
}

std::vector<uint16_t> monoPcm16ToRawAdc(const AudioBuffer& buffer) {
    std::vector<uint16_t> raw;
    if (buffer.channels == 0) return raw;
    const size_t frames = buffer.samples.size() / buffer.channels;
    raw.reserve(frames);
    for (size_t i = 0; i < frames; ++i) raw.push_back(pcm16ToRawAdc(buffer.samples[i * buffer.channels]));
    return raw;
}

void stereoPcm16ToRawAdc(const AudioBuffer& buffer, std::vector<uint16_t>& ch0, std::vector<uint16_t>& ch1) {
    ch0.clear();
    ch1.clear();
    if (buffer.channels == 0) return;
    const size_t frames = buffer.samples.size() / buffer.channels;
    ch0.reserve(frames);
    ch1.reserve(frames);
    for (size_t i = 0; i < frames; ++i) {
        ch0.push_back(pcm16ToRawAdc(buffer.samples[i * buffer.channels]));
        ch1.push_back(pcm16ToRawAdc(buffer.samples[i * buffer.channels + (buffer.channels > 1 ? 1 : 0)]));
    }
}

AudioBuffer floatMonoToPcm16(const std::vector<float>& samples, uint32_t sample_rate_hz) {
    AudioBuffer b;
    b.sample_rate_hz = std::max<uint32_t>(1, sample_rate_hz);
    b.channels = 1;
    b.samples.reserve(samples.size());
    for (float s : samples) {
        const double v = std::max(-1.0, std::min(1.0, static_cast<double>(s)));
        b.samples.push_back(clamp16(static_cast<int32_t>(std::lrint(v * 32767.0))));
    }
    return b;
}

std::vector<float> monoPcm16ToFloat(const AudioBuffer& buffer) {
    std::vector<float> out;
    if (buffer.channels == 0) return out;
    const size_t frames = buffer.samples.size() / buffer.channels;
    out.reserve(frames);
    for (size_t i = 0; i < frames; ++i) out.push_back(static_cast<float>(static_cast<double>(buffer.samples[i * buffer.channels]) / 32768.0));
    return out;
}

void applyEffectsToRawAdcMono(std::vector<uint16_t>& raw, uint32_t sample_rate_hz, const std::vector<std::string>& effect_ids) {
    if (raw.empty() || effect_ids.empty()) return;
    auto b = rawAdcToMonoPcm16(raw, sample_rate_hz);
    applyEffects(b, effect_ids);
    raw = monoPcm16ToRawAdc(b);
}

void applyEffectsToFloatMono(std::vector<float>& samples, uint32_t sample_rate_hz, const std::vector<std::string>& effect_ids) {
    if (samples.empty() || effect_ids.empty()) return;
    auto b = floatMonoToPcm16(samples, sample_rate_hz);
    applyEffects(b, effect_ids);
    samples = monoPcm16ToFloat(b);
}

void applyEffectsToStereoChannels(std::vector<uint16_t>& ch0, std::vector<uint16_t>& ch1, uint32_t sample_rate_hz,
                                  const std::vector<std::string>& effects_ch0, const std::vector<std::string>& effects_ch1) {
    if (!effects_ch0.empty()) applyEffectsToRawAdcMono(ch0, sample_rate_hz, effects_ch0);
    if (!effects_ch1.empty()) applyEffectsToRawAdcMono(ch1, sample_rate_hz, effects_ch1);
}

} // namespace AudioFilters
