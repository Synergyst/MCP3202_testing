#pragma once

#include "GwProtocol.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace gw {

struct StreamInfo {
    uint16_t stream_id = 0;
    uint8_t stream_class = GW_STREAM_CLASS_CONTROL;
    uint8_t channel_count = 0;
    uint8_t sample_format = 0;
    uint32_t sample_rate_hz = 0;
    bool active = false;
};

struct DeviceStatus {
    bool protocol_active = false;
    bool connected = false;
    std::string protocol_name = "ADC2";
    std::string transport_name;
    uint64_t packets_ok = 0;
    uint64_t packets_crc_bad = 0;
    uint64_t sequence_gaps = 0;
    uint32_t firmware_lost_frames = 0;
    uint32_t firmware_flags = 0;
    uint32_t declared_rate_hz = 0;
    uint8_t sample_format = GW_SAMPLE_U16_LE;
    uint8_t channel_count = 2;
    uint16_t adc_stream_id = GW_STREAM_ADC0;
    uint32_t caps_max_rate_hz = 0;
    uint32_t caps_formats = 0;
};

class DeviceSession {
public:
    DeviceStatus status;
    uint32_t nextPacketSeq() { return packet_seq_++; }
    uint16_t nextRequestId() { return request_id_++; }
private:
    uint32_t packet_seq_ = 1;
    uint16_t request_id_ = 1;
};

} // namespace gw
