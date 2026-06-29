#include "GwProtocol.hpp"

#include <cstring>

namespace gw {

uint32_t crc32Update(uint32_t crc, const uint8_t* data, size_t len) {
    crc = ~crc;
    for (size_t i = 0; i < len; ++i) {
        crc ^= data[i];
        for (int b = 0; b < 8; ++b) {
            crc = (crc >> 1) ^ (0xEDB88320u & (-(int32_t)(crc & 1u)));
        }
    }
    return ~crc;
}

uint32_t crc32(const uint8_t* data, size_t len) {
    return crc32Update(0, data, len);
}

std::vector<uint8_t> encodePacket(uint8_t msg_type,
                                  uint16_t stream_id,
                                  uint32_t seq,
                                  const std::vector<uint8_t>& payload,
                                  uint8_t flags) {
    gw_header_t h{};
    h.magic = GW_MAGIC;
    h.version = GW_VERSION;
    h.header_len = GW_HEADER_LEN;
    h.msg_type = msg_type;
    h.flags = flags;
    h.stream_id = stream_id;
    h.payload_len = static_cast<uint16_t>(payload.size());
    h.seq = seq;

    std::vector<uint8_t> out(sizeof(h) + payload.size() + sizeof(uint32_t));
    std::memcpy(out.data(), &h, sizeof(h));
    if (!payload.empty()) std::memcpy(out.data() + sizeof(h), payload.data(), payload.size());
    uint32_t c = 0;
    c = crc32Update(c, out.data(), sizeof(h) + payload.size());
    std::memcpy(out.data() + sizeof(h) + payload.size(), &c, sizeof(c));
    return out;
}

std::vector<uint8_t> encodeControlRequest(uint16_t opcode,
                                           uint16_t request_id,
                                           const void* args,
                                           uint16_t arg_len,
                                           uint32_t packet_seq) {
    gw_ctrl_req_payload_t req{};
    req.opcode = opcode;
    req.request_id = request_id;
    req.arg_len = arg_len;
    std::vector<uint8_t> payload(sizeof(req) + arg_len);
    std::memcpy(payload.data(), &req, sizeof(req));
    if (args && arg_len) std::memcpy(payload.data() + sizeof(req), args, arg_len);
    return encodePacket(GW_MSG_CTRL_REQ, GW_STREAM_CONTROL, packet_seq, payload);
}

std::vector<uint8_t> encodeSetSampleRate(uint32_t sample_rate_hz,
                                          uint16_t request_id,
                                          uint32_t packet_seq) {
    return encodeControlRequest(GW_OP_SET_SAMPLE_RATE, request_id, &sample_rate_hz, sizeof(sample_rate_hz), packet_seq);
}

bool validatePacket(const gw_header_t& header,
                    const std::vector<uint8_t>& payload,
                    uint32_t received_crc) {
    if (header.magic != GW_MAGIC || header.version != GW_VERSION || header.header_len != GW_HEADER_LEN) return false;
    if (header.payload_len != payload.size()) return false;
    uint32_t c = 0;
    c = crc32Update(c, reinterpret_cast<const uint8_t*>(&header), sizeof(header));
    if (!payload.empty()) c = crc32Update(c, payload.data(), payload.size());
    return c == received_crc;
}

std::string msgTypeName(uint8_t msg_type) {
    switch (msg_type) {
        case GW_MSG_DATA: return "DATA";
        case GW_MSG_CTRL_REQ: return "CTRL_REQ";
        case GW_MSG_CTRL_RESP: return "CTRL_RESP";
        case GW_MSG_STATUS: return "STATUS";
        case GW_MSG_EVENT: return "EVENT";
        case GW_MSG_HELLO: return "HELLO";
        case GW_MSG_CAPS: return "CAPS";
        case GW_MSG_TIME_SYNC: return "TIME_SYNC";
        default: return "UNKNOWN";
    }
}

std::string sampleFormatName(uint8_t sample_format) {
    switch (sample_format) {
        case GW_SAMPLE_U8: return "U8";
        case GW_SAMPLE_S8: return "S8";
        case GW_SAMPLE_U16_LE: return "U16_LE";
        case GW_SAMPLE_S16_LE: return "S16_LE";
        case GW_SAMPLE_U24_LE: return "U24_LE";
        case GW_SAMPLE_S24_LE: return "S24_LE";
        case GW_SAMPLE_U32_LE: return "U32_LE";
        case GW_SAMPLE_S32_LE: return "S32_LE";
        case GW_SAMPLE_F32_LE: return "F32_LE";
        case GW_SAMPLE_PACKED_U12_LE: return "PACKED_U12_LE";
        case GW_SAMPLE_PACKED_U10_LE: return "PACKED_U10_LE";
        default: return "UNKNOWN";
    }
}

size_t bytesPerSample(uint8_t sample_format, uint8_t channel_count, uint16_t frame_count) {
    const size_t frames = frame_count;
    const size_t channels = channel_count;
    switch (sample_format) {
        case GW_SAMPLE_U8:
        case GW_SAMPLE_S8:
            return frames * channels;
        case GW_SAMPLE_U16_LE:
        case GW_SAMPLE_S16_LE:
            return frames * channels * 2;
        case GW_SAMPLE_U24_LE:
        case GW_SAMPLE_S24_LE:
            return frames * channels * 3;
        case GW_SAMPLE_U32_LE:
        case GW_SAMPLE_S32_LE:
        case GW_SAMPLE_F32_LE:
            return frames * channels * 4;
        case GW_SAMPLE_PACKED_U12_LE:
            return (frames * channels * 12 + 7) / 8;
        case GW_SAMPLE_PACKED_U10_LE:
            return (frames * channels * 10 + 7) / 8;
        default:
            return 0;
    }
}

} // namespace gw
