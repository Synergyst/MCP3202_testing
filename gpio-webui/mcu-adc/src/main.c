#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>

#include "pico/stdlib.h"
#include "pico/stdio.h"
#include "pico/stdio_usb.h"
#include "pico/multicore.h"
#include "hardware/gpio.h"
#include "hardware/spi.h"
#include "hardware/sync.h"

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

#define PROTO_MAGIC 0x32434441u /* 'ADC2' little-endian */
#define PROTO_VERSION 1u
#define FLAG_OVERFLOW 0x00000001u
#define FLAG_COUNTER_GAP 0x00000002u

#if MCU_ADC_SPI_PORT == 0
#define ADC_SPI spi0
#else
#define ADC_SPI spi1
#endif

typedef struct __attribute__((packed)) {
    uint32_t seq;
    uint16_t ch0;
    uint16_t ch1;
} sample_frame_t;

typedef struct __attribute__((packed)) {
    uint32_t magic;
    uint16_t version;
    uint16_t header_bytes;
    uint32_t sample_rate_hz;
    uint32_t frame_count;
    uint32_t sequence_start;
    uint32_t flags;
    uint32_t lost_frames;
    uint32_t reserved;
} packet_header_t;

static sample_frame_t ring[MCU_ADC_RING_FRAMES];
static volatile uint32_t wr_idx = 0;
static volatile uint32_t rd_idx = 0;
static volatile uint32_t next_seq = 0;
static volatile uint32_t lost_frames = 0;
static volatile uint32_t overflow_events = 0;

static inline void cs_select(void) {
    gpio_put(MCU_ADC_PIN_CS, 0);
    // MCP3202 tSUCS min is 100 ns. One short nop sequence is ample at RP2040 clocks.
    __asm volatile("nop\n nop\n nop\n");
}

static inline void cs_deselect(void) {
    gpio_put(MCU_ADC_PIN_CS, 1);
    // MCP3202 tCSH min is 500 ns. This conservative delay also guarantees high between channels.
    sleep_us(1);
}

static uint16_t mcp3202_read_channel(uint channel) {
    // Datasheet MCU-SPI framing, mode 0, 8-bit segments:
    // byte0: 00000001 (7 leading zeros + start)
    // byte1: SGL=1, ODD=channel, MSBF=1, don't cares
    // byte2: don't cares. RX byte1 low nibble=B11..B8, RX byte2=B7..B0.
    uint8_t tx[3] = {0x01u, (uint8_t)(0xA0u | (channel ? 0x40u : 0x00u)), 0x00u};
    uint8_t rx[3] = {0, 0, 0};
    cs_select();
    spi_write_read_blocking(ADC_SPI, tx, rx, 3);
    cs_deselect();
    return (uint16_t)(((rx[1] & 0x0Fu) << 8) | rx[2]);
}

static void push_sample(uint16_t ch0, uint16_t ch1) {
    uint32_t w = wr_idx;
    uint32_t n = (w + 1u) & (MCU_ADC_RING_FRAMES - 1u);
    if (n == rd_idx) {
        // Drop oldest frame to keep stream current. Host will see seq gap/lost count.
        rd_idx = (rd_idx + 1u) & (MCU_ADC_RING_FRAMES - 1u);
        lost_frames++;
        overflow_events++;
    }
    ring[w].seq = next_seq++;
    ring[w].ch0 = ch0;
    ring[w].ch1 = ch1;
    __dmb();
    wr_idx = n;
}

static void sampler_core(void) {
    const uint32_t period_us = 1000000u / MCU_ADC_SAMPLE_RATE_HZ;
    absolute_time_t next = get_absolute_time();
    while (true) {
        next = delayed_by_us(next, period_us);
        sleep_until(next);
        uint16_t ch0 = mcp3202_read_channel(0);
        uint16_t ch1 = mcp3202_read_channel(1);
        push_sample(ch0, ch1);
    }
}

static uint32_t ring_available(void) {
    uint32_t w = wr_idx;
    uint32_t r = rd_idx;
    return (w - r) & (MCU_ADC_RING_FRAMES - 1u);
}

static bool pop_frame(sample_frame_t *out) {
    if (rd_idx == wr_idx) return false;
    *out = ring[rd_idx];
    __dmb();
    rd_idx = (rd_idx + 1u) & (MCU_ADC_RING_FRAMES - 1u);
    return true;
}

static uint32_t crc32_update(uint32_t crc, const uint8_t *data, size_t len) {
    crc = ~crc;
    for (size_t i = 0; i < len; ++i) {
        crc ^= data[i];
        for (int b = 0; b < 8; ++b) {
            crc = (crc >> 1) ^ (0xEDB88320u & (-(int32_t)(crc & 1u)));
        }
    }
    return ~crc;
}

static void write_all(const void *data, size_t len) {
    const uint8_t *p = (const uint8_t *)data;
    while (len) {
        int c = getchar_timeout_us(0);
        (void)c; // keep USB stdio serviced on some SDK versions
        size_t chunk = len > 64 ? 64 : len;
        fwrite(p, 1, chunk, stdout);
        p += chunk;
        len -= chunk;
    }
}

int main(void) {
    stdio_init_all();
    // This is a binary stream; disable Pico stdio's default LF -> CRLF translation.
    stdio_set_translate_crlf(&stdio_usb, false);
    sleep_ms(1000);

    // SPI mode 0. MCP3202 supports SPI modes 0,0 and 1,1; mode 0 matches our frame code.
    spi_init(ADC_SPI, MCU_ADC_SPI_BAUD);
    spi_set_format(ADC_SPI, 8, SPI_CPOL_0, SPI_CPHA_0, SPI_MSB_FIRST);
    gpio_set_function(MCU_ADC_PIN_MISO, GPIO_FUNC_SPI);
    gpio_set_function(MCU_ADC_PIN_SCK, GPIO_FUNC_SPI);
    gpio_set_function(MCU_ADC_PIN_MOSI, GPIO_FUNC_SPI);
    gpio_init(MCU_ADC_PIN_CS);
    gpio_set_dir(MCU_ADC_PIN_CS, GPIO_OUT);
    gpio_put(MCU_ADC_PIN_CS, 1);

    multicore_launch_core1(sampler_core);

    sample_frame_t packet_frames[MCU_ADC_PACKET_FRAMES];
    uint32_t last_sent_seq = 0;
    bool have_last = false;

    while (true) {
        uint32_t avail = ring_available();
        if (avail < MCU_ADC_PACKET_FRAMES) {
            sleep_ms(1);
            continue;
        }

        uint32_t n = 0;
        while (n < MCU_ADC_PACKET_FRAMES && pop_frame(&packet_frames[n])) n++;
        if (n == 0) continue;

        uint32_t flags = 0;
        if (overflow_events) flags |= FLAG_OVERFLOW;
        if (have_last && packet_frames[0].seq != last_sent_seq + 1u) flags |= FLAG_COUNTER_GAP;
        have_last = true;
        last_sent_seq = packet_frames[n - 1].seq;

        packet_header_t hdr;
        hdr.magic = PROTO_MAGIC;
        hdr.version = PROTO_VERSION;
        hdr.header_bytes = sizeof(packet_header_t);
        hdr.sample_rate_hz = MCU_ADC_SAMPLE_RATE_HZ;
        hdr.frame_count = n;
        hdr.sequence_start = packet_frames[0].seq;
        hdr.flags = flags;
        hdr.lost_frames = lost_frames;
        hdr.reserved = 0;

        uint32_t crc = 0;
        crc = crc32_update(crc, (const uint8_t *)&hdr, sizeof(hdr));
        crc = crc32_update(crc, (const uint8_t *)packet_frames, n * sizeof(sample_frame_t));

        write_all(&hdr, sizeof(hdr));
        write_all(packet_frames, n * sizeof(sample_frame_t));
        write_all(&crc, sizeof(crc));
        fflush(stdout);
    }
}
