#pragma once

/*
 * GWP1 - GPIO Web Peripheral Protocol
 * Transport-agnostic little-endian binary packet framework for ADC/DAC/control/status/event streams.
 *
 * Packet layout:
 *   gw_header_t header
 *   uint8_t payload[header.payload_len]
 *   uint32_t crc32_le  // CRC32 over header + payload, initial value 0
 *
 * All multi-byte fields are little-endian on the wire. Current targets are little-endian.
 */

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define GW_MAGIC 0x31505747u /* 'GWP1' little-endian */
#define GW_VERSION 1u
#define GW_HEADER_LEN 16u
#define GW_MAX_PAYLOAD_LEN 4096u

#if defined(__GNUC__) || defined(__clang__)
#define GW_PACKED __attribute__((packed))
#else
#define GW_PACKED
#endif

typedef enum {
    GW_MSG_DATA      = 0x01,
    GW_MSG_CTRL_REQ  = 0x02,
    GW_MSG_CTRL_RESP = 0x03,
    GW_MSG_STATUS    = 0x04,
    GW_MSG_EVENT     = 0x05,
    GW_MSG_HELLO     = 0x06,
    GW_MSG_CAPS      = 0x07,
    GW_MSG_TIME_SYNC = 0x08
} gw_msg_type_t;

typedef enum {
    GW_STREAM_CONTROL = 0,
    GW_STREAM_ADC0    = 1,
    GW_STREAM_ADC1    = 2,
    GW_STREAM_DAC0    = 100,
    GW_STREAM_DAC1    = 101
} gw_stream_id_t;

typedef enum {
    GW_STREAM_CLASS_CONTROL = 0,
    GW_STREAM_CLASS_ADC     = 1,
    GW_STREAM_CLASS_DAC     = 2,
    GW_STREAM_CLASS_GPIO    = 3,
    GW_STREAM_CLASS_EVENT   = 4
} gw_stream_class_t;

typedef enum {
    GW_SAMPLE_U8            = 1,
    GW_SAMPLE_S8            = 2,
    GW_SAMPLE_U16_LE        = 3,
    GW_SAMPLE_S16_LE        = 4,
    GW_SAMPLE_U24_LE        = 5,
    GW_SAMPLE_S24_LE        = 6,
    GW_SAMPLE_U32_LE        = 7,
    GW_SAMPLE_S32_LE        = 8,
    GW_SAMPLE_F32_LE        = 9,
    GW_SAMPLE_PACKED_U12_LE = 10,
    GW_SAMPLE_PACKED_U10_LE = 11
} gw_sample_format_t;

typedef enum {
    GW_STATUS_OK               = 0,
    GW_STATUS_ERROR            = 1,
    GW_STATUS_BAD_REQUEST      = 2,
    GW_STATUS_UNSUPPORTED      = 3,
    GW_STATUS_INVALID_ARGUMENT = 4,
    GW_STATUS_BUSY             = 5
} gw_status_code_t;

typedef enum {
    GW_OP_HELLO             = 0x0001,
    GW_OP_GET_CAPS          = 0x0002,
    GW_OP_GET_STATUS        = 0x0003,
    GW_OP_STREAM_START      = 0x0010,
    GW_OP_STREAM_STOP       = 0x0011,
    GW_OP_SET_SAMPLE_RATE   = 0x0020,
    GW_OP_SET_PACKET_FRAMES = 0x0021,
    GW_OP_SET_SAMPLE_FORMAT = 0x0022,
    GW_OP_SET_CHANNELS      = 0x0023,
    GW_OP_TIME_SYNC         = 0x0030,
    GW_OP_DAC_SET_RATE      = 0x0100,
    GW_OP_DAC_SET_FORMAT    = 0x0101,
    GW_OP_DAC_WRITE_FRAME   = 0x0102,
    GW_OP_DAC_STREAM_START  = 0x0103,
    GW_OP_DAC_STREAM_STOP   = 0x0104,
    GW_OP_DAC_FLUSH         = 0x0105,
    GW_OP_DAC_WRITE_BLOCK   = 0x0106,
    GW_OP_DAC_GET_STATUS    = 0x0107
} gw_opcode_t;

typedef enum {
    GW_EVENT_OVERFLOW        = 1,
    GW_EVENT_COUNTER_GAP     = 2,
    GW_EVENT_STREAM_STARTED  = 3,
    GW_EVENT_STREAM_STOPPED  = 4,
    GW_EVENT_RATE_CHANGED    = 5,
    GW_EVENT_DAC_UNDERRUN    = 100,
    GW_EVENT_DAC_FLUSHED     = 101,
    GW_EVENT_DRIVER_FAULT    = 200
} gw_event_code_t;

typedef enum {
    GW_FLAG_NONE       = 0x00,
    GW_FLAG_MORE       = 0x01,
    GW_FLAG_EVENT      = 0x02,
    GW_FLAG_ERROR      = 0x04,
    GW_FLAG_CRC_OPTION = 0x80
} gw_packet_flags_t;

typedef struct GW_PACKED {
    uint32_t magic;
    uint8_t  version;
    uint8_t  header_len;
    uint8_t  msg_type;
    uint8_t  flags;
    uint16_t stream_id;
    uint16_t payload_len;
    uint32_t seq;
} gw_header_t;

typedef struct GW_PACKED {
    uint32_t frame_start;
    uint16_t frame_count;
    uint8_t  channel_count;
    uint8_t  sample_format;
} gw_adc_data_payload_t;

typedef struct GW_PACKED {
    uint32_t frame_start;
    uint16_t frame_count;
    uint8_t  channel_count;
    uint8_t  sample_format;
} gw_dac_data_payload_t;

typedef struct GW_PACKED {
    uint32_t sample_rate_hz;
} gw_rate_payload_t;

typedef struct GW_PACKED {
    uint8_t channel_count;
    uint8_t sample_format;
    uint8_t reserved0;
    uint8_t reserved1;
} gw_format_payload_t;

typedef struct GW_PACKED {
    uint16_t opcode;
    uint16_t request_id;
    uint16_t arg_len;
    uint16_t reserved;
} gw_ctrl_req_payload_t;

typedef struct GW_PACKED {
    uint16_t opcode;
    uint16_t request_id;
    uint16_t status;
    uint16_t resp_len;
} gw_ctrl_resp_payload_t;

typedef struct GW_PACKED {
    uint32_t uptime_ms;
    uint32_t configured_sample_rate_hz;
    uint32_t measured_sample_rate_hz;
    uint32_t lost_frames;
    uint32_t overflow_events;
    uint32_t packets_sent;
    uint32_t packets_received;
    uint32_t crc_errors_rx;
    uint32_t flags;
    uint16_t stream_id;
    uint16_t buffer_fill_frames;
    uint8_t  stream_class;
    uint8_t  channel_count;
    uint8_t  sample_format;
    uint8_t  active;
} gw_status_payload_t;

typedef struct GW_PACKED {
    uint16_t event_code;
    uint16_t stream_id;
    uint32_t value0;
    uint32_t value1;
} gw_event_payload_t;

typedef struct GW_PACKED {
    uint32_t frame_seq;
    uint64_t device_time_us;
} gw_time_sync_payload_t;

typedef struct GW_PACKED {
    uint32_t protocol_version;
    uint32_t firmware_version;
    uint32_t device_class_mask;
    uint32_t supported_sample_formats;
    uint32_t max_sample_rate_hz;
    uint16_t max_payload_len;
    uint16_t preferred_packet_frames;
    uint8_t  adc_streams;
    uint8_t  dac_streams;
    uint8_t  gpio_streams;
    uint8_t  reserved;
} gw_caps_payload_t;

#define GW_DEVICE_CLASS_ADC  (1u << 0)
#define GW_DEVICE_CLASS_DAC  (1u << 1)
#define GW_DEVICE_CLASS_GPIO (1u << 2)

#define GW_SAMPLE_FORMAT_MASK(fmt) (1u << ((uint32_t)(fmt) & 31u))

#if defined(__cplusplus)
static_assert(sizeof(gw_header_t) == GW_HEADER_LEN, "gw_header_t must be 16 bytes");
static_assert(sizeof(gw_adc_data_payload_t) == 8, "gw_adc_data_payload_t must be 8 bytes");
static_assert(sizeof(gw_dac_data_payload_t) == 8, "gw_dac_data_payload_t must be 8 bytes");
static_assert(sizeof(gw_rate_payload_t) == 4, "gw_rate_payload_t must be 4 bytes");
static_assert(sizeof(gw_format_payload_t) == 4, "gw_format_payload_t must be 4 bytes");
static_assert(sizeof(gw_ctrl_req_payload_t) == 8, "gw_ctrl_req_payload_t must be 8 bytes");
static_assert(sizeof(gw_ctrl_resp_payload_t) == 8, "gw_ctrl_resp_payload_t must be 8 bytes");
static_assert(sizeof(gw_time_sync_payload_t) == 12, "gw_time_sync_payload_t must be 12 bytes");
#endif

#ifdef __cplusplus
}
#endif
