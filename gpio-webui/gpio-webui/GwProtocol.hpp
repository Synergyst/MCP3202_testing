#pragma once

#include "../protocol/gw_protocol.h"

#include <cstdint>
#include <string>
#include <vector>

namespace gw {

constexpr uint32_t Adc2Magic = 0x32434441u; // 'ADC2'

uint32_t crc32Update(uint32_t crc, const uint8_t* data, size_t len);
uint32_t crc32(const uint8_t* data, size_t len);

struct Packet {
    gw_header_t header{};
    std::vector<uint8_t> payload;
    uint32_t crc = 0;
};

std::vector<uint8_t> encodePacket(uint8_t msg_type,
                                  uint16_t stream_id,
                                  uint32_t seq,
                                  const std::vector<uint8_t>& payload,
                                  uint8_t flags = 0);

std::vector<uint8_t> encodeControlRequest(uint16_t opcode,
                                           uint16_t request_id,
                                           const void* args,
                                           uint16_t arg_len,
                                           uint32_t packet_seq = 0);

std::vector<uint8_t> encodeSetSampleRate(uint32_t sample_rate_hz,
                                          uint16_t request_id = 1,
                                          uint32_t packet_seq = 0);

bool validatePacket(const gw_header_t& header,
                    const std::vector<uint8_t>& payload,
                    uint32_t received_crc);

std::string msgTypeName(uint8_t msg_type);
std::string sampleFormatName(uint8_t sample_format);
size_t bytesPerSample(uint8_t sample_format, uint8_t channel_count, uint16_t frame_count);

} // namespace gw
