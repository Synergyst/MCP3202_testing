#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace Fsk {

struct DecoderSettings {
    double mark_hz = 1200.0;
    double space_hz = 2200.0;
    double baud = 1200.0;
    bool normalize = true;
    double normalize_headroom_db = 6.0;
    double extra_gain_db = 12.0;
    bool dc_block = true;
};

struct DecodeResult {
    bool has_bits = false;
    int selected_channel = -1; // 0=CH0, 1=CH1, 2=mix
    double confidence = 0.0;
    double demod_confidence = 0.0;
    double frame_score = 0.0;
    std::vector<int> bits;
    std::vector<uint8_t> bytes;
    std::string raw_bits;
    std::string raw_bytes_hex;
};

class FskDecoder {
public:
    DecodeResult decodeBest(const std::vector<uint16_t>& ch0,
                            const std::vector<uint16_t>& ch1,
                            const std::vector<int>& sources,
                            uint32_t sample_rate_hz,
                            const DecoderSettings& settings,
                            size_t max_raw_bits_shown = 1200) const;
};

std::string bytesToHex(const std::vector<uint8_t>& bytes);
std::string bitsToString(const std::vector<int>& bits, size_t max_bits = 1200);

} // namespace Fsk
