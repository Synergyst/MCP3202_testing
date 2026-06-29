#pragma once

#include "GwProtocol.hpp"
#include <cstdint>
#include <cstring>
#include <vector>

namespace gw {

struct DacStreamConfig {
    uint16_t stream_id = GW_STREAM_DAC0;
    uint32_t sample_rate_hz = 48000;
    uint8_t channel_count = 1;
    uint8_t sample_format = GW_SAMPLE_U16_LE;
};

inline std::vector<uint8_t> encodeDacData(const DacStreamConfig& cfg,
                                          uint32_t frame_start,
                                          const std::vector<uint8_t>& sample_bytes,
                                          uint16_t frame_count,
                                          uint32_t packet_seq) {
    gw_dac_data_payload_t meta{};
    meta.frame_start = frame_start;
    meta.frame_count = frame_count;
    meta.channel_count = cfg.channel_count;
    meta.sample_format = cfg.sample_format;
    std::vector<uint8_t> payload(sizeof(meta) + sample_bytes.size());
    std::memcpy(payload.data(), &meta, sizeof(meta));
    if (!sample_bytes.empty()) std::memcpy(payload.data() + sizeof(meta), sample_bytes.data(), sample_bytes.size());
    return encodePacket(GW_MSG_DATA, cfg.stream_id, packet_seq, payload);
}

} // namespace gw
