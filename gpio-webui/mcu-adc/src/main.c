#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <stdlib.h>
#include <math.h>

#include "pico/stdlib.h"
#include "pico/stdio.h"
#include "pico/stdio_usb.h"
#include "pico/multicore.h"
#include "hardware/gpio.h"
#include "hardware/spi.h"
#include "hardware/sync.h"
#include "../../protocol/gw_protocol.h"

#ifndef MCU_ADC_SAMPLE_RATE_HZ
#define MCU_ADC_SAMPLE_RATE_HZ 16000u
#endif
#ifndef MCU_ADC_SPI_BAUD
#define MCU_ADC_SPI_BAUD 1600000u
#endif
#ifndef MCU_ADC_SPI_PORT
#define MCU_ADC_SPI_PORT 0
#endif
#ifndef MCU_ADC_PIN_MISO
#define MCU_ADC_PIN_MISO 0u
#endif
#ifndef MCU_ADC_PIN_CS
#define MCU_ADC_PIN_CS 1u
#endif
#ifndef MCU_ADC_PIN_SCK
#define MCU_ADC_PIN_SCK 2u
#endif
#ifndef MCU_ADC_PIN_MOSI
#define MCU_ADC_PIN_MOSI 3u
#endif
#ifndef MCU_ADC_PACKET_FRAMES
#define MCU_ADC_PACKET_FRAMES 128u
#endif
#ifndef MCU_ADC_RING_FRAMES
#define MCU_ADC_RING_FRAMES 4096u
#endif

#ifndef MCU_DAC_ENABLE
#define MCU_DAC_ENABLE 1
#endif
#ifndef MCU_DAC_SPI_BAUD
#define MCU_DAC_SPI_BAUD 10000000u
#endif
#ifndef MCU_DAC_SPI_PORT
#define MCU_DAC_SPI_PORT 1
#endif
#ifndef MCU_DAC_PIN_CS
#define MCU_DAC_PIN_CS 13u
#endif
#ifndef MCU_DAC_PIN_SCK
#define MCU_DAC_PIN_SCK 14u
#endif
#ifndef MCU_DAC_PIN_MOSI
#define MCU_DAC_PIN_MOSI 15u
#endif
#ifndef MCU_DAC_PIN_LDAC
#define MCU_DAC_PIN_LDAC -1
#endif
#ifndef MCU_DAC_PIN_SHDN
#define MCU_DAC_PIN_SHDN -1
#endif
#ifndef MCU_DAC_DEFAULT_RATE_HZ
#define MCU_DAC_DEFAULT_RATE_HZ 48000u
#endif
#ifndef MCU_DAC_MAX_RATE_HZ
#define MCU_DAC_MAX_RATE_HZ 100000u
#endif

#if defined(PICO_RP2350) || defined(PICO_PLATFORM_RP2350)
#define GW_PLATFORM_HAS_FPU 1
#define GW_PLATFORM_ID 2350u
#else
#define GW_PLATFORM_HAS_FPU 0
#define GW_PLATFORM_ID 2040u
#endif

#define FLAG_OVERFLOW 0x00000001u
#define FLAG_COUNTER_GAP 0x00000002u
#define FLAG_DAC_UNDERRUN 0x00010000u
#define FLAG_DAC_ERROR 0x00020000u

#if MCU_ADC_SPI_PORT == 0
#define ADC_SPI spi0
#else
#define ADC_SPI spi1
#endif

#if MCU_DAC_SPI_PORT == 0
#define DAC_SPI spi0
#else
#define DAC_SPI spi1
#endif

typedef struct __attribute__((packed)) { uint16_t ch0; uint16_t ch1; } adc_pair_u16_t;
typedef struct __attribute__((packed)) { uint16_t ch0; uint16_t ch1; } dac_pair_u16_t;

typedef struct {
    uint8_t channel_count;
    uint8_t resolution_bits;
    uint32_t min_rate_hz;
    uint32_t max_rate_hz;
    uint32_t supported_formats;
} stream_driver_caps_t;

static adc_pair_u16_t ring[MCU_ADC_RING_FRAMES];
static volatile uint32_t wr_idx = 0;
static volatile uint32_t rd_idx = 0;
static volatile uint32_t next_seq = 0;
static volatile uint32_t rd_seq = 0;
static volatile uint32_t lost_frames = 0;
static volatile uint32_t overflow_events = 0;
static volatile uint32_t g_sample_rate = MCU_ADC_SAMPLE_RATE_HZ;
static volatile bool stream_active = true;
static uint32_t gw_packet_seq = 1;
static uint32_t packets_sent = 0;
static uint32_t packets_received = 0;
static uint32_t crc_errors_rx = 0;
static uint32_t last_status_ms = 0;

static volatile uint32_t dac_sample_rate = MCU_DAC_DEFAULT_RATE_HZ;
static volatile uint8_t dac_channel_count = 2;
static volatile uint8_t dac_sample_format = GW_SAMPLE_U16_LE;
static volatile bool dac_stream_active = false;
static uint32_t dac_frames_written = 0;
static uint32_t dac_blocks_received = 0;
static uint32_t dac_underruns = 0;
static uint32_t dac_last_frame_ms = 0;
static uint16_t dac_last_a = 0;
static uint16_t dac_last_b = 0;

#define DTMF_MAX_DIGITS 64u
#define DTMF_PHASE_IDLE 0u
#define DTMF_PHASE_TONE 1u
#define DTMF_PHASE_GAP  2u

static volatile bool dtmf_active = false;
static char dtmf_digits[DTMF_MAX_DIGITS];
static uint8_t dtmf_digit_count = 0;
static uint8_t dtmf_index = 0;
static uint8_t dtmf_phase = DTMF_PHASE_IDLE;
static uint16_t dtmf_tone_ms = 100;
static uint16_t dtmf_gap_ms = 50;
static uint16_t dtmf_amplitude = 1200;
static uint8_t dtmf_channel_mask = GW_DAC_CHANNEL_A;
static uint32_t dtmf_phase_until_ms = 0;
static uint32_t dtmf_next_sample_us = 0;
static uint32_t dtmf_generated_frames = 0;
static float dtmf_phase_row = 0.0f;
static float dtmf_phase_col = 0.0f;
static float dtmf_inc_row = 0.0f;
static float dtmf_inc_col = 0.0f;

static inline void adc_cs_select(void) { gpio_put(MCU_ADC_PIN_CS, 0); __asm volatile("nop\n nop\n nop\n"); }
static inline void adc_cs_deselect(void) { gpio_put(MCU_ADC_PIN_CS, 1); sleep_us(1); }

static inline void dac_cs_select(void) { gpio_put(MCU_DAC_PIN_CS, 0); __asm volatile("nop\n nop\n nop\n"); }
static inline void dac_cs_deselect(void) { gpio_put(MCU_DAC_PIN_CS, 1); __asm volatile("nop\n nop\n nop\n"); }

static uint16_t mcp3202_read_channel(uint channel) {
    uint8_t tx[3] = {0x01u, (uint8_t)(0xA0u | (channel ? 0x40u : 0x00u)), 0x00u};
    uint8_t rx[3] = {0, 0, 0};
    adc_cs_select(); spi_write_read_blocking(ADC_SPI, tx, rx, 3); adc_cs_deselect();
    return (uint16_t)(((rx[1] & 0x0Fu) << 8) | rx[2]);
}

static void adc_get_caps(stream_driver_caps_t *caps) {
    caps->channel_count = 2;
    caps->resolution_bits = 12;
    caps->min_rate_hz = 1;
    caps->max_rate_hz = 100000;
    caps->supported_formats = GW_SAMPLE_FORMAT_MASK(GW_SAMPLE_U16_LE) | GW_SAMPLE_FORMAT_MASK(GW_SAMPLE_PACKED_U12_LE);
}

static void dac_get_caps(stream_driver_caps_t *caps) {
    caps->channel_count = 2;
    caps->resolution_bits = 12;
    caps->min_rate_hz = 1;
    caps->max_rate_hz = MCU_DAC_MAX_RATE_HZ;
    caps->supported_formats = GW_SAMPLE_FORMAT_MASK(GW_SAMPLE_U16_LE) | GW_SAMPLE_FORMAT_MASK(GW_SAMPLE_PACKED_U12_LE);
}

static uint16_t mcp4922_build_command(uint8_t channel, uint16_t code) {
    uint16_t cmd = (uint16_t)(code & 0x0FFFu);
    if (channel) cmd |= 0x8000u;       // A/B: 1=DACB, 0=DACA
    cmd |= 0x1000u;                    // SHDN: 1=active
    cmd |= 0x2000u;                    // GA: 1=1x gain
    // BUF remains 0: unbuffered VREF, matching MCP4922 POR/default behavior.
    return cmd;
}

static void mcp4922_write_command(uint16_t command) {
#if MCU_DAC_ENABLE
    uint8_t tx[2] = {(uint8_t)(command >> 8), (uint8_t)(command & 0xFFu)};
    dac_cs_select();
    spi_write_blocking(DAC_SPI, tx, 2);
    dac_cs_deselect();
#else
    (void)command;
#endif
}

static void mcp4922_latch(void) {
#if MCU_DAC_ENABLE
#if MCU_DAC_PIN_LDAC >= 0
    gpio_put(MCU_DAC_PIN_LDAC, 0);
    sleep_us(1);
    gpio_put(MCU_DAC_PIN_LDAC, 1);
#endif
#endif
}

static void mcp4922_write_pair(uint16_t a, uint16_t b) {
    dac_last_a = a & 0x0FFFu;
    dac_last_b = b & 0x0FFFu;
    mcp4922_write_command(mcp4922_build_command(0, dac_last_a));
    mcp4922_write_command(mcp4922_build_command(1, dac_last_b));
    mcp4922_latch();
    dac_frames_written++;
    dac_last_frame_ms = to_ms_since_boot(get_absolute_time());
}

static void write_packet(uint8_t type, uint16_t stream, const void *payload, uint16_t payload_len);
static void send_event(uint16_t code, uint16_t stream_id, uint32_t value0, uint32_t value1);

static bool dtmf_digit_freqs(char d, float *row, float *col) {
    if (d >= 'a' && d <= 'd') d = (char)(d - 'a' + 'A');
    switch (d) {
        case '1': *row = 697.0f; *col = 1209.0f; return true;
        case '2': *row = 697.0f; *col = 1336.0f; return true;
        case '3': *row = 697.0f; *col = 1477.0f; return true;
        case 'A': *row = 697.0f; *col = 1633.0f; return true;
        case '4': *row = 770.0f; *col = 1209.0f; return true;
        case '5': *row = 770.0f; *col = 1336.0f; return true;
        case '6': *row = 770.0f; *col = 1477.0f; return true;
        case 'B': *row = 770.0f; *col = 1633.0f; return true;
        case '7': *row = 852.0f; *col = 1209.0f; return true;
        case '8': *row = 852.0f; *col = 1336.0f; return true;
        case '9': *row = 852.0f; *col = 1477.0f; return true;
        case 'C': *row = 852.0f; *col = 1633.0f; return true;
        case '*': *row = 941.0f; *col = 1209.0f; return true;
        case '0': *row = 941.0f; *col = 1336.0f; return true;
        case '#': *row = 941.0f; *col = 1477.0f; return true;
        case 'D': *row = 941.0f; *col = 1633.0f; return true;
        default: return false;
    }
}

static void dtmf_prepare_digit(void) {
    float row = 0.0f, col = 0.0f;
    if (dtmf_index >= dtmf_digit_count || !dtmf_digit_freqs(dtmf_digits[dtmf_index], &row, &col)) {
        dtmf_active = false; dtmf_phase = DTMF_PHASE_IDLE; mcp4922_write_pair(0, 0); return;
    }
    const float two_pi = 6.2831853071795864769f;
    uint32_t rate = dac_sample_rate ? dac_sample_rate : MCU_DAC_DEFAULT_RATE_HZ;
    dtmf_phase_row = 0.0f; dtmf_phase_col = 0.0f;
    dtmf_inc_row = two_pi * row / (float)rate;
    dtmf_inc_col = two_pi * col / (float)rate;
    uint32_t now_ms = to_ms_since_boot(get_absolute_time());
    dtmf_phase = DTMF_PHASE_TONE;
    dtmf_phase_until_ms = now_ms + dtmf_tone_ms;
    dtmf_next_sample_us = to_us_since_boot(get_absolute_time());
}

static void dtmf_stop(void) {
    dtmf_active = false; dtmf_phase = DTMF_PHASE_IDLE; dtmf_index = 0; dtmf_digit_count = 0;
    mcp4922_write_pair(0, 0);
}

static uint16_t dtmf_start(const gw_dtmf_play_payload_t *p) {
#if !MCU_DAC_ENABLE
    (void)p; return GW_STATUS_UNSUPPORTED;
#else
    if (p->digit_count == 0 || p->digit_count > DTMF_MAX_DIGITS) return GW_STATUS_INVALID_ARGUMENT;
    uint8_t n = 0;
    for (uint8_t i = 0; i < p->digit_count && i < DTMF_MAX_DIGITS; ++i) {
        float r, c;
        if (dtmf_digit_freqs(p->digits[i], &r, &c)) dtmf_digits[n++] = p->digits[i];
    }
    if (!n) return GW_STATUS_INVALID_ARGUMENT;
    dtmf_digit_count = n; dtmf_index = 0;
    dtmf_tone_ms = p->tone_ms ? p->tone_ms : 100;
    dtmf_gap_ms = p->gap_ms ? p->gap_ms : 50;
    dtmf_amplitude = p->amplitude > 2047 ? 2047 : p->amplitude;
    dtmf_channel_mask = (p->channel_mask & GW_DAC_CHANNEL_STEREO) ? (p->channel_mask & GW_DAC_CHANNEL_STEREO) : GW_DAC_CHANNEL_A;
    dtmf_generated_frames = 0; dtmf_active = true;
    dtmf_prepare_digit();
    return GW_STATUS_OK;
#endif
}

static void dtmf_tick(void) {
#if MCU_DAC_ENABLE
    if (!dtmf_active) return;
    uint32_t now_ms = to_ms_since_boot(get_absolute_time());
    if (dtmf_phase == DTMF_PHASE_TONE && now_ms >= dtmf_phase_until_ms) {
        mcp4922_write_pair(0, 0);
        if (dtmf_gap_ms == 0) { dtmf_index++; dtmf_prepare_digit(); return; }
        dtmf_phase = DTMF_PHASE_GAP; dtmf_phase_until_ms = now_ms + dtmf_gap_ms;
        return;
    }
    if (dtmf_phase == DTMF_PHASE_GAP && now_ms >= dtmf_phase_until_ms) {
        dtmf_index++;
        if (dtmf_index >= dtmf_digit_count) { dtmf_stop(); return; }
        dtmf_prepare_digit(); return;
    }
    if (dtmf_phase != DTMF_PHASE_TONE) return;
    uint32_t now_us = to_us_since_boot(get_absolute_time());
    uint32_t rate = dac_sample_rate ? dac_sample_rate : MCU_DAC_DEFAULT_RATE_HZ;
    uint32_t period_us = 1000000u / rate; if (!period_us) period_us = 1;
    int budget = 8;
    while ((int32_t)(now_us - dtmf_next_sample_us) >= 0 && budget-- > 0 && dtmf_active && dtmf_phase == DTMF_PHASE_TONE) {
        float y = (sinf(dtmf_phase_row) + sinf(dtmf_phase_col)) * 0.5f;
        int32_t centered = 2048 + (int32_t)(y * (float)dtmf_amplitude);
        if (centered < 0) centered = 0; if (centered > 4095) centered = 4095;
        uint16_t a = (dtmf_channel_mask & GW_DAC_CHANNEL_A) ? (uint16_t)centered : 0;
        uint16_t b = (dtmf_channel_mask & GW_DAC_CHANNEL_B) ? (uint16_t)centered : 0;
        mcp4922_write_pair(a, b);
        dtmf_phase_row += dtmf_inc_row; if (dtmf_phase_row >= 6.2831853f) dtmf_phase_row -= 6.2831853f;
        dtmf_phase_col += dtmf_inc_col; if (dtmf_phase_col >= 6.2831853f) dtmf_phase_col -= 6.2831853f;
        dtmf_next_sample_us += period_us; dtmf_generated_frames++;
    }
#endif
}

static void send_dtmf_status(void) {
    gw_dtmf_status_payload_t st = {0};
    st.active = dtmf_active ? 1 : 0; st.channel_mask = dtmf_channel_mask; st.current_index = dtmf_index; st.digit_count = dtmf_digit_count;
    st.current_digit = (dtmf_index < dtmf_digit_count) ? dtmf_digits[dtmf_index] : 0; st.phase = dtmf_phase;
    uint32_t now_ms = to_ms_since_boot(get_absolute_time()); st.remaining_ms = (dtmf_phase_until_ms > now_ms) ? (uint16_t)(dtmf_phase_until_ms - now_ms) : 0;
    st.generated_frames = dtmf_generated_frames;
    write_packet(GW_MSG_STATUS, GW_STREAM_DAC0, &st, sizeof(st));
}

static void push_sample(uint16_t ch0, uint16_t ch1) {
    uint32_t w = wr_idx;
    uint32_t n = (w + 1u) & (MCU_ADC_RING_FRAMES - 1u);
    if (n == rd_idx) { rd_idx = (rd_idx + 1u) & (MCU_ADC_RING_FRAMES - 1u); rd_seq++; lost_frames++; overflow_events++; }
    ring[w].ch0 = ch0; ring[w].ch1 = ch1; next_seq++;
    __dmb(); wr_idx = n;
}

static void sampler_core(void) {
    absolute_time_t next = get_absolute_time();
    while (true) {
        uint32_t rate = g_sample_rate;
        uint32_t period_us = 1000000u / (rate > 0 ? rate : 1);
        next = delayed_by_us(next, period_us);
        sleep_until(next);
        if (!stream_active) continue;
        push_sample(mcp3202_read_channel(0), mcp3202_read_channel(1));
    }
}

static uint32_t ring_available(void) { return (wr_idx - rd_idx) & (MCU_ADC_RING_FRAMES - 1u); }
static bool pop_pair(adc_pair_u16_t *out) {
    if (rd_idx == wr_idx) return false;
    *out = ring[rd_idx]; __dmb(); rd_idx = (rd_idx + 1u) & (MCU_ADC_RING_FRAMES - 1u); rd_seq++; return true;
}

static uint32_t crc32_update(uint32_t crc, const uint8_t *data, size_t len) {
    crc = ~crc;
    for (size_t i = 0; i < len; ++i) { crc ^= data[i]; for (int b = 0; b < 8; ++b) crc = (crc >> 1) ^ (0xEDB88320u & (-(int32_t)(crc & 1u))); }
    return ~crc;
}

static void write_all(const void *data, size_t len) { const uint8_t *p = (const uint8_t *)data; while (len) { size_t chunk = len > 64 ? 64 : len; fwrite(p, 1, chunk, stdout); p += chunk; len -= chunk; } }
static void write_packet(uint8_t type, uint16_t stream, const void *payload, uint16_t payload_len) {
    gw_header_t h = {0}; h.magic = GW_MAGIC; h.version = GW_VERSION; h.header_len = sizeof(gw_header_t); h.msg_type = type; h.stream_id = stream; h.payload_len = payload_len; h.seq = gw_packet_seq++;
    uint32_t crc = 0; crc = crc32_update(crc, (const uint8_t *)&h, sizeof(h)); if (payload_len) crc = crc32_update(crc, (const uint8_t *)payload, payload_len);
    write_all(&h, sizeof(h)); if (payload_len) write_all(payload, payload_len); write_all(&crc, sizeof(crc)); fflush(stdout); packets_sent++;
}

static void send_event(uint16_t code, uint16_t stream_id, uint32_t value0, uint32_t value1) {
    gw_event_payload_t e = {0}; e.event_code = code; e.stream_id = stream_id; e.value0 = value0; e.value1 = value1;
    write_packet(GW_MSG_EVENT, stream_id, &e, sizeof(e));
}

static void send_caps(void) {
    stream_driver_caps_t ac; adc_get_caps(&ac);
    stream_driver_caps_t dc; dac_get_caps(&dc);
    gw_caps_payload_t c = {0};
    c.protocol_version = GW_VERSION;
    c.firmware_version = 2;
    c.device_class_mask = GW_DEVICE_CLASS_ADC;
#if MCU_DAC_ENABLE
    c.device_class_mask |= GW_DEVICE_CLASS_DAC;
#endif
    c.supported_sample_formats = ac.supported_formats;
#if MCU_DAC_ENABLE
    c.supported_sample_formats |= dc.supported_formats;
#endif
    c.max_sample_rate_hz = ac.max_rate_hz > dc.max_rate_hz ? ac.max_rate_hz : dc.max_rate_hz;
    c.max_payload_len = GW_MAX_PAYLOAD_LEN;
    c.preferred_packet_frames = MCU_ADC_PACKET_FRAMES;
    c.adc_streams = 1;
#if MCU_DAC_ENABLE
    c.dac_streams = 1;
#endif
    write_packet(GW_MSG_CAPS, GW_STREAM_CONTROL, &c, sizeof(c));
}

static void send_adc_status(void) {
    gw_status_payload_t s = {0};
    s.uptime_ms = to_ms_since_boot(get_absolute_time()); s.configured_sample_rate_hz = g_sample_rate; s.measured_sample_rate_hz = g_sample_rate; s.lost_frames = lost_frames; s.overflow_events = overflow_events; s.packets_sent = packets_sent; s.packets_received = packets_received; s.crc_errors_rx = crc_errors_rx; s.flags = overflow_events ? FLAG_OVERFLOW : 0; s.stream_id = GW_STREAM_ADC0; s.buffer_fill_frames = ring_available(); s.stream_class = GW_STREAM_CLASS_ADC; s.channel_count = 2; s.sample_format = GW_SAMPLE_U16_LE; s.active = stream_active ? 1 : 0;
    write_packet(GW_MSG_STATUS, GW_STREAM_ADC0, &s, sizeof(s));
}

static void send_dac_status(void) {
    gw_status_payload_t s = {0};
    s.uptime_ms = to_ms_since_boot(get_absolute_time()); s.configured_sample_rate_hz = dac_sample_rate; s.measured_sample_rate_hz = dac_sample_rate; s.lost_frames = dac_underruns; s.overflow_events = 0; s.packets_sent = packets_sent; s.packets_received = packets_received; s.crc_errors_rx = crc_errors_rx; s.flags = dac_underruns ? FLAG_DAC_UNDERRUN : 0; s.stream_id = GW_STREAM_DAC0; s.buffer_fill_frames = 0; s.stream_class = GW_STREAM_CLASS_DAC; s.channel_count = dac_channel_count; s.sample_format = dac_sample_format; s.active = dac_stream_active ? 1 : 0;
    write_packet(GW_MSG_STATUS, GW_STREAM_DAC0, &s, sizeof(s));
}

static void send_status(void) {
    send_adc_status();
#if MCU_DAC_ENABLE
    send_dac_status();
#endif
}

static void send_ctrl_resp(uint16_t opcode, uint16_t reqid, uint16_t status) {
    gw_ctrl_resp_payload_t r = {0}; r.opcode = opcode; r.request_id = reqid; r.status = status; r.resp_len = 0;
    write_packet(GW_MSG_CTRL_RESP, GW_STREAM_CONTROL, &r, sizeof(r));
}

static void handle_ascii_rate(void) {
    char buf[16]; int pos = 0;
    while (pos < 15) { int b = getchar_timeout_us(1000); if (b == '\n' || b == '\r' || b == EOF) break; if (b >= '0' && b <= '9') buf[pos++] = (char)b; else break; }
    if (pos > 0) { buf[pos] = '\0'; uint32_t nr = (uint32_t)strtoul(buf, NULL, 10); if (nr > 0 && nr <= 100000) g_sample_rate = nr; }
}

static uint16_t handle_dac_block(const uint8_t *payload, uint16_t payload_len) {
#if !MCU_DAC_ENABLE
    (void)payload; (void)payload_len;
    return GW_STATUS_UNSUPPORTED;
#else
    if (payload_len < sizeof(gw_dac_data_payload_t)) return GW_STATUS_BAD_REQUEST;
    gw_dac_data_payload_t meta; memcpy(&meta, payload, sizeof(meta));
    const uint8_t *samples = payload + sizeof(meta);
    uint16_t sample_len = payload_len - sizeof(meta);
    if (meta.channel_count < 1 || meta.channel_count > 2) return GW_STATUS_INVALID_ARGUMENT;
    if (meta.sample_format != GW_SAMPLE_U16_LE && meta.sample_format != GW_SAMPLE_PACKED_U12_LE) return GW_STATUS_UNSUPPORTED;

    if (meta.sample_format == GW_SAMPLE_U16_LE) {
        uint16_t bytes_per_frame = (uint16_t)(meta.channel_count * 2u);
        if (sample_len < (uint16_t)(meta.frame_count * bytes_per_frame)) return GW_STATUS_BAD_REQUEST;
        for (uint16_t i = 0; i < meta.frame_count; ++i) {
            uint16_t a = 0, b = dac_last_b;
            memcpy(&a, samples + i * bytes_per_frame, 2);
            if (meta.channel_count > 1) memcpy(&b, samples + i * bytes_per_frame + 2, 2); else b = a;
            mcp4922_write_pair(a & 0x0FFFu, b & 0x0FFFu);
        }
    } else {
        uint16_t bytes_per_frame = (uint16_t)(meta.channel_count * 2u); // unpacked little-endian 12-bit words for now.
        if (sample_len < (uint16_t)(meta.frame_count * bytes_per_frame)) return GW_STATUS_BAD_REQUEST;
        for (uint16_t i = 0; i < meta.frame_count; ++i) {
            uint16_t a = 0, b = dac_last_b;
            memcpy(&a, samples + i * bytes_per_frame, 2);
            if (meta.channel_count > 1) memcpy(&b, samples + i * bytes_per_frame + 2, 2); else b = a;
            mcp4922_write_pair(a & 0x0FFFu, b & 0x0FFFu);
        }
    }
    dac_blocks_received++;
    return GW_STATUS_OK;
#endif
}

static uint16_t handle_dac_ctrl(const gw_ctrl_req_payload_t *req, const uint8_t *args) {
#if !MCU_DAC_ENABLE
    (void)req; (void)args;
    return GW_STATUS_UNSUPPORTED;
#else
    if (req->opcode == GW_OP_DAC_SET_RATE) {
        if (req->arg_len < sizeof(gw_rate_payload_t)) return GW_STATUS_BAD_REQUEST;
        gw_rate_payload_t r; memcpy(&r, args, sizeof(r));
        if (r.sample_rate_hz < 1 || r.sample_rate_hz > MCU_DAC_MAX_RATE_HZ) return GW_STATUS_INVALID_ARGUMENT;
        dac_sample_rate = r.sample_rate_hz;
        send_event(GW_EVENT_RATE_CHANGED, GW_STREAM_DAC0, r.sample_rate_hz, 0);
        return GW_STATUS_OK;
    }
    if (req->opcode == GW_OP_DAC_SET_FORMAT) {
        if (req->arg_len < sizeof(gw_format_payload_t)) return GW_STATUS_BAD_REQUEST;
        gw_format_payload_t f; memcpy(&f, args, sizeof(f));
        if (f.channel_count < 1 || f.channel_count > 2) return GW_STATUS_INVALID_ARGUMENT;
        if (f.sample_format != GW_SAMPLE_U16_LE && f.sample_format != GW_SAMPLE_PACKED_U12_LE) return GW_STATUS_UNSUPPORTED;
        dac_channel_count = f.channel_count;
        dac_sample_format = f.sample_format;
        return GW_STATUS_OK;
    }
    if (req->opcode == GW_OP_DAC_WRITE_FRAME) {
        if (req->arg_len < 2) return GW_STATUS_BAD_REQUEST;
        uint16_t a = 0, b = dac_last_b;
        memcpy(&a, args, 2);
        if (req->arg_len >= 4) memcpy(&b, args + 2, 2); else b = a;
        mcp4922_write_pair(a, b);
        return GW_STATUS_OK;
    }
    if (req->opcode == GW_OP_DAC_WRITE_BLOCK) {
        return handle_dac_block(args, req->arg_len);
    }
    if (req->opcode == GW_OP_DAC_STREAM_START) {
        dac_stream_active = true;
        dac_last_frame_ms = to_ms_since_boot(get_absolute_time());
        send_event(GW_EVENT_STREAM_STARTED, GW_STREAM_DAC0, 0, 0);
        return GW_STATUS_OK;
    }
    if (req->opcode == GW_OP_DAC_STREAM_STOP) {
        dac_stream_active = false;
        send_event(GW_EVENT_STREAM_STOPPED, GW_STREAM_DAC0, 0, 0);
        return GW_STATUS_OK;
    }
    if (req->opcode == GW_OP_DAC_FLUSH) {
        dac_last_a = dac_last_b = 0;
        mcp4922_write_pair(0, 0);
        send_event(GW_EVENT_DAC_FLUSHED, GW_STREAM_DAC0, 0, 0);
        return GW_STATUS_OK;
    }
    if (req->opcode == GW_OP_DAC_GET_STATUS) {
        send_dac_status();
        return GW_STATUS_OK;
    }
    if (req->opcode == GW_OP_DAC_DTMF_PLAY) {
        if (req->arg_len < sizeof(gw_dtmf_play_payload_t)) return GW_STATUS_BAD_REQUEST;
        gw_dtmf_play_payload_t p; memcpy(&p, args, sizeof(p));
        return dtmf_start(&p);
    }
    if (req->opcode == GW_OP_DAC_DTMF_STOP) {
        dtmf_stop();
        send_dtmf_status();
        return GW_STATUS_OK;
    }
    if (req->opcode == GW_OP_DAC_DTMF_STATUS) {
        send_dtmf_status();
        return GW_STATUS_OK;
    }
    return GW_STATUS_UNSUPPORTED;
#endif
}

static void handle_gwp1_packet(uint8_t first_magic_byte) {
    uint8_t hdrb[sizeof(gw_header_t)]; hdrb[0] = first_magic_byte;
    for (uint i = 1; i < sizeof(gw_header_t); ++i) { int b = getchar_timeout_us(1000); if (b == EOF) return; hdrb[i] = (uint8_t)b; }
    gw_header_t h; memcpy(&h, hdrb, sizeof(h));
    if (h.magic != GW_MAGIC || h.version != GW_VERSION || h.header_len != sizeof(gw_header_t) || h.payload_len > GW_MAX_PAYLOAD_LEN) return;
    static uint8_t payload[GW_MAX_PAYLOAD_LEN]; for (uint i = 0; i < h.payload_len; ++i) { int b = getchar_timeout_us(1000); if (b == EOF) return; payload[i] = (uint8_t)b; }
    uint32_t rxcrc = 0; for (uint i = 0; i < 4; ++i) { int b = getchar_timeout_us(1000); if (b == EOF) return; rxcrc |= ((uint32_t)(uint8_t)b) << (8u * i); }
    uint32_t calc = 0; calc = crc32_update(calc, hdrb, sizeof(h)); calc = crc32_update(calc, payload, h.payload_len); if (calc != rxcrc) { crc_errors_rx++; return; }
    packets_received++;

    if (h.msg_type == GW_MSG_DATA && h.stream_id == GW_STREAM_DAC0) {
        uint16_t status = dac_stream_active ? handle_dac_block(payload, h.payload_len) : GW_STATUS_BUSY;
        if (status != GW_STATUS_OK) send_event(GW_EVENT_DRIVER_FAULT, GW_STREAM_DAC0, status, h.seq);
        return;
    }
    if (h.msg_type != GW_MSG_CTRL_REQ || h.payload_len < sizeof(gw_ctrl_req_payload_t)) return;

    gw_ctrl_req_payload_t req; memcpy(&req, payload, sizeof(req));
    if ((uint32_t)sizeof(req) + req.arg_len > h.payload_len) { send_ctrl_resp(req.opcode, req.request_id, GW_STATUS_BAD_REQUEST); return; }
    const uint8_t *args = payload + sizeof(req);
    uint16_t status = GW_STATUS_OK;
    if (req.opcode == GW_OP_SET_SAMPLE_RATE && req.arg_len >= 4) { uint32_t nr; memcpy(&nr, args, 4); if (nr > 0 && nr <= 100000) g_sample_rate = nr; else status = GW_STATUS_INVALID_ARGUMENT; }
    else if (req.opcode == GW_OP_STREAM_START) stream_active = true;
    else if (req.opcode == GW_OP_STREAM_STOP) stream_active = false;
    else if (req.opcode == GW_OP_GET_STATUS) { send_status(); }
    else if (req.opcode == GW_OP_GET_CAPS || req.opcode == GW_OP_HELLO) { send_caps(); }
    else if ((req.opcode >= GW_OP_DAC_SET_RATE && req.opcode <= GW_OP_DAC_GET_STATUS) || (req.opcode >= GW_OP_DAC_DTMF_PLAY && req.opcode <= GW_OP_DAC_DTMF_STATUS)) { status = handle_dac_ctrl(&req, args); }
    else status = GW_STATUS_UNSUPPORTED;
    send_ctrl_resp(req.opcode, req.request_id, status);
}

static void check_dac_underrun(void) {
#if MCU_DAC_ENABLE
    if (!dac_stream_active) return;
    uint32_t now = to_ms_since_boot(get_absolute_time());
    uint32_t rate = dac_sample_rate ? dac_sample_rate : 1;
    uint32_t grace_ms = 2000u / rate;
    if (grace_ms < 10u) grace_ms = 10u;
    if (dac_last_frame_ms != 0 && now - dac_last_frame_ms > grace_ms) {
        dac_underruns++;
        dac_last_frame_ms = now;
        send_event(GW_EVENT_DAC_UNDERRUN, GW_STREAM_DAC0, dac_underruns, 0);
    }
#endif
}

int main(void) {
    stdio_init_all(); stdio_set_translate_crlf(&stdio_usb, false); sleep_ms(1000);
    spi_init(ADC_SPI, MCU_ADC_SPI_BAUD); spi_set_format(ADC_SPI, 8, SPI_CPOL_0, SPI_CPHA_0, SPI_MSB_FIRST);
    gpio_set_function(MCU_ADC_PIN_MISO, GPIO_FUNC_SPI); gpio_set_function(MCU_ADC_PIN_SCK, GPIO_FUNC_SPI); gpio_set_function(MCU_ADC_PIN_MOSI, GPIO_FUNC_SPI);
    gpio_init(MCU_ADC_PIN_CS); gpio_set_dir(MCU_ADC_PIN_CS, GPIO_OUT); gpio_put(MCU_ADC_PIN_CS, 1);
#if MCU_DAC_ENABLE
    spi_init(DAC_SPI, MCU_DAC_SPI_BAUD); spi_set_format(DAC_SPI, 8, SPI_CPOL_0, SPI_CPHA_0, SPI_MSB_FIRST);
    gpio_set_function(MCU_DAC_PIN_SCK, GPIO_FUNC_SPI); gpio_set_function(MCU_DAC_PIN_MOSI, GPIO_FUNC_SPI);
    gpio_init(MCU_DAC_PIN_CS); gpio_set_dir(MCU_DAC_PIN_CS, GPIO_OUT); gpio_put(MCU_DAC_PIN_CS, 1);
#if MCU_DAC_PIN_LDAC >= 0
    gpio_init(MCU_DAC_PIN_LDAC); gpio_set_dir(MCU_DAC_PIN_LDAC, GPIO_OUT); gpio_put(MCU_DAC_PIN_LDAC, 1);
#endif
#if MCU_DAC_PIN_SHDN >= 0
    gpio_init(MCU_DAC_PIN_SHDN); gpio_set_dir(MCU_DAC_PIN_SHDN, GPIO_OUT); gpio_put(MCU_DAC_PIN_SHDN, 1);
#endif
    // Define DAC outputs immediately at boot. This writes active-mode zero to
    // both MCP4922 input registers; with LDAC tied low, outputs update on CS rise.
    mcp4922_write_pair(0, 0);
#endif
    multicore_launch_core1(sampler_core);
    send_caps(); send_status();

    adc_pair_u16_t packet_pairs[MCU_ADC_PACKET_FRAMES];
    while (true) {
        int c = getchar_timeout_us(0); if (c == 'S') handle_ascii_rate(); else if (c == 'G') handle_gwp1_packet((uint8_t)c);
        uint32_t now_ms = to_ms_since_boot(get_absolute_time()); if (now_ms - last_status_ms >= 1000u) { last_status_ms = now_ms; send_status(); }
        dtmf_tick();
        check_dac_underrun();
        if (ring_available() < MCU_ADC_PACKET_FRAMES) { sleep_ms(1); continue; }
        uint32_t start_seq = rd_seq; uint32_t n = 0; while (n < MCU_ADC_PACKET_FRAMES && pop_pair(&packet_pairs[n])) n++; if (!n) continue;
        struct __attribute__((packed)) { gw_adc_data_payload_t meta; adc_pair_u16_t samples[MCU_ADC_PACKET_FRAMES]; } pkt;
        pkt.meta.frame_start = start_seq; pkt.meta.frame_count = n; pkt.meta.channel_count = 2; pkt.meta.sample_format = GW_SAMPLE_U16_LE; memcpy(pkt.samples, packet_pairs, n * sizeof(adc_pair_u16_t));
        write_packet(GW_MSG_DATA, GW_STREAM_ADC0, &pkt, sizeof(gw_adc_data_payload_t) + n * sizeof(adc_pair_u16_t));
    }
}
