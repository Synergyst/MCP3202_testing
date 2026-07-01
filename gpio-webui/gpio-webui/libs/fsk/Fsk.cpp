#include "Fsk.hpp"

#include <algorithm>
#include <cmath>
#include <iomanip>
#include <sstream>

namespace {

std::vector<double> makeSignal(const std::vector<uint16_t>& ch0, const std::vector<uint16_t>& ch1, int source, const Fsk::DecoderSettings& settings) {
    const size_t n = std::min(ch0.size(), ch1.size());
    std::vector<double> x(n, 0.0);
    if (n == 0) return x;

    double mean = 0.0;
    for (size_t i = 0; i < n; ++i) {
        double v = 0.0;
        if (source == 0) v = static_cast<double>(ch0[i]) - 2048.0;
        else if (source == 1) v = static_cast<double>(ch1[i]) - 2048.0;
        else v = ((static_cast<double>(ch0[i]) + static_cast<double>(ch1[i])) * 0.5) - 2048.0;
        x[i] = v;
        mean += v;
    }

    if (settings.dc_block) {
        mean /= static_cast<double>(n);
        for (auto& v : x) v -= mean;
    }

    double scale = std::pow(10.0, settings.extra_gain_db / 20.0);
    if (settings.normalize) {
        double peak = 0.0;
        for (double v : x) peak = std::max(peak, std::abs(v));
        if (peak > 0.0) {
            const double target_peak = 2047.0 * std::pow(10.0, -std::max(0.0, settings.normalize_headroom_db) / 20.0);
            scale *= target_peak / peak;
        }
    }
    for (auto& v : x) v = std::max(-2047.0, std::min(2047.0, v * scale));
    return x;
}

double tonePower(const std::vector<double>& x, size_t begin, size_t end, double hz, double sr) {
    double re = 0.0, im = 0.0;
    const double w = 2.0 * M_PI * hz / sr;
    for (size_t n = begin; n < end; ++n) {
        const double a = w * static_cast<double>(n - begin);
        re += x[n] * std::cos(a);
        im -= x[n] * std::sin(a);
    }
    return re * re + im * im;
}

struct BitDemodResult {
    std::vector<int> bits;
    double confidence = 0.0;
};

BitDemodResult demodBits(const std::vector<double>& x, uint32_t sr, double phase_samples, const Fsk::DecoderSettings& settings) {
    BitDemodResult r;
    const double spb = static_cast<double>(sr) / std::max(1.0, settings.baud);
    if (x.size() < static_cast<size_t>(spb * 20.0)) return r;
    const size_t nbits = static_cast<size_t>((static_cast<double>(x.size()) - phase_samples) / spb);
    r.bits.reserve(nbits);
    double sep_sum = 0.0;
    size_t sep_count = 0;
    for (size_t bit = 0; bit < nbits; ++bit) {
        const size_t begin = static_cast<size_t>(std::llround(phase_samples + static_cast<double>(bit) * spb));
        const size_t end = std::min(x.size(), static_cast<size_t>(std::llround(phase_samples + static_cast<double>(bit + 1) * spb)));
        if (end <= begin + 2) break;
        const double p_mark = tonePower(x, begin, end, settings.mark_hz, sr);
        const double p_space = tonePower(x, begin, end, settings.space_hz, sr);
        r.bits.push_back(p_mark >= p_space ? 1 : 0);
        const double denom = std::max(1.0, p_mark + p_space);
        sep_sum += std::abs(p_mark - p_space) / denom;
        sep_count++;
    }
    r.confidence = sep_count ? sep_sum / static_cast<double>(sep_count) : 0.0;
    return r;
}

struct FrameDecode {
    std::vector<uint8_t> bytes;
    double frame_score = 0.0;
};

FrameDecode decodeFrames(const std::vector<int>& bits) {
    FrameDecode best;
    for (size_t offset = 0; offset + 10 <= bits.size(); ++offset) {
        FrameDecode cur;
        size_t pos = offset;
        while (pos + 10 <= bits.size()) {
            if (bits[pos] == 0 && bits[pos + 9] == 1) {
                uint8_t v = 0;
                for (int b = 0; b < 8; ++b) if (bits[pos + 1 + static_cast<size_t>(b)]) v |= static_cast<uint8_t>(1u << b);
                cur.bytes.push_back(v);
                pos += 10;
            } else {
                if (cur.bytes.size() > best.bytes.size()) best = cur;
                cur = FrameDecode();
                pos += 1;
            }
        }
        if (cur.bytes.size() > best.bytes.size()) best = cur;
    }
    best.frame_score = bits.empty() ? 0.0 : static_cast<double>(best.bytes.size() * 10) / static_cast<double>(bits.size());
    return best;
}

} // namespace

namespace Fsk {

std::string bytesToHex(const std::vector<uint8_t>& bytes) {
    std::ostringstream oss;
    oss << std::hex << std::uppercase << std::setfill('0');
    for (size_t i = 0; i < bytes.size(); ++i) {
        if (i) oss << ' ';
        oss << std::setw(2) << static_cast<int>(bytes[i]);
    }
    return oss.str();
}

std::string bitsToString(const std::vector<int>& bits, size_t max_bits) {
    std::string s;
    const size_t n = std::min(max_bits, bits.size());
    s.reserve(n);
    for (size_t i = 0; i < n; ++i) s.push_back(bits[i] ? '1' : '0');
    return s;
}

DecodeResult FskDecoder::decodeBest(const std::vector<uint16_t>& ch0,
                                    const std::vector<uint16_t>& ch1,
                                    const std::vector<int>& sources,
                                    uint32_t sample_rate_hz,
                                    const DecoderSettings& settings,
                                    size_t max_raw_bits_shown) const {
    DecodeResult best;
    const uint32_t sr = std::max<uint32_t>(1, sample_rate_hz);
    const double spb = static_cast<double>(sr) / std::max(1.0, settings.baud);
    const int phases = std::max(1, static_cast<int>(std::ceil(spb)) * 2);

    for (int source : sources) {
        auto x = makeSignal(ch0, ch1, source, settings);
        for (int pi = 0; pi < phases; ++pi) {
            const double phase = (static_cast<double>(pi) / static_cast<double>(phases)) * spb;
            auto demod = demodBits(x, sr, phase, settings);
            auto frames_dec = decodeFrames(demod.bits);
            DecodeResult c;
            c.has_bits = !demod.bits.empty();
            c.selected_channel = source;
            c.demod_confidence = demod.confidence;
            c.frame_score = frames_dec.frame_score;
            c.confidence = (demod.confidence * 0.65) + (frames_dec.frame_score * 0.35);
            c.bits = std::move(demod.bits);
            c.bytes = std::move(frames_dec.bytes);
            c.raw_bytes_hex = bytesToHex(c.bytes);
            c.raw_bits = bitsToString(c.bits, max_raw_bits_shown);
            if (c.confidence > best.confidence || (!best.has_bits && c.has_bits)) best = std::move(c);
        }
    }
    return best;
}

} // namespace Fsk
