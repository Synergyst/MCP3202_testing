# GWP1 - GPIO Web Peripheral Protocol

GWP1 is a transport-agnostic little-endian binary protocol for modular source/sink devices. It is intended to carry high-rate ADC/DAC data with minimal overhead while moving configuration, telemetry, and diagnostics to control/status/event packets.

## Goals

- Binary-only long-term protocol.
- Transport agnostic: USB CDC, UART, TCP, Unix sockets, future MCU/SBC links.
- Device/driver agnostic: ADC, DAC, GPIO, telephony, future modules.
- Minimal high-rate data packet overhead.
- No redundant per-sample sequence numbers.
- No slow-changing telemetry in high-rate data packets.
- Runtime tunables via binary control packets.
- Capability discovery so host can consume/produce common stream types from different devices.
- Integer/fixed-point friendly firmware paths for RP2040; optional FPU-capable paths for RP2350/SBC.

## Packet Layout

```text
gw_header_t header
uint8_t payload[header.payload_len]
uint32_t crc32_le
```

CRC32 is computed over `header + payload` with initial value 0. All multi-byte integers are little-endian.

## Common Header

Defined in `protocol/gw_protocol.h`:

```c
typedef struct __attribute__((packed)) {
    uint32_t magic;       // 'GWP1' little-endian: 0x31505747
    uint8_t  version;     // 1
    uint8_t  header_len;  // 16
    uint8_t  msg_type;
    uint8_t  flags;
    uint16_t stream_id;
    uint16_t payload_len;
    uint32_t seq;         // packet/message sequence
} gw_header_t;
```

## Message Classes

- `DATA`: high-rate source/sink payloads.
- `CTRL_REQ`: binary host/device command request.
- `CTRL_RESP`: command response with request id.
- `STATUS`: low-rate telemetry or on-demand status.
- `EVENT`: errors, overflows, stream changes.
- `HELLO` / `CAPS`: discovery and capabilities.
- `TIME_SYNC`: optional timing anchors without bloating every data packet.

## Stream IDs

Stream IDs identify logical endpoints inside a device/session. Transport/session identity identifies the physical or network peer.

Suggested reserved IDs:

```text
0      control/default
1      ADC0
2      ADC1
100    DAC0
101    DAC1
```

## ADC Data Payload

```c
typedef struct __attribute__((packed)) {
    uint32_t frame_start;
    uint16_t frame_count;
    uint8_t  channel_count;
    uint8_t  sample_format;
} gw_adc_data_payload_t;
```

Samples follow immediately. Initial MCP3202 format uses `GW_SAMPLE_U16_LE` with interleaved channels:

```text
frame0_ch0, frame0_ch1, frame1_ch0, frame1_ch1, ...
```

At 128 dual-channel frames:

```text
16-byte common header
8-byte ADC data payload header
512-byte U16 sample payload
4-byte CRC32
= 540 bytes
```

The legacy ADC2 packet is 1060 bytes for the same data, so GWP1 roughly halves wire bandwidth.

## DAC Data Payload

DAC uses the same structure as ADC but flows host-to-device:

```c
typedef struct __attribute__((packed)) {
    uint32_t frame_start;
    uint16_t frame_count;
    uint8_t  channel_count;
    uint8_t  sample_format;
} gw_dac_data_payload_t;
```

Samples follow immediately. For the RP2040 MCP4922 backend, `GW_SAMPLE_U16_LE` carries one little-endian 16-bit word per channel per frame. The low 12 bits are used as the MCP4922 code; upper bits are ignored. With two channels:

```text
frame0_daca, frame0_dacb, frame1_daca, frame1_dacb, ...
```

DAC `DATA` packets are accepted on stream `GW_STREAM_DAC0` after `GW_OP_DAC_STREAM_START`. One-shot writes can use `GW_OP_DAC_WRITE_FRAME`; block writes can use either `DATA` on DAC0 or `GW_OP_DAC_WRITE_BLOCK` with the same `gw_dac_data_payload_t + samples` payload.

DAC status reports queue/fill equivalent, underruns, rate, format, active state, packet counters, CRC errors, and flags. The current RP2040 implementation writes samples synchronously as packets arrive; it therefore reports `buffer_fill_frames=0` and underrun events when a running stream is not fed within the firmware grace window.

## Control

Control request:

```c
typedef struct __attribute__((packed)) {
    uint16_t opcode;
    uint16_t request_id;
    uint16_t arg_len;
    uint16_t reserved;
} gw_ctrl_req_payload_t;
```

Control response:

```c
typedef struct __attribute__((packed)) {
    uint16_t opcode;
    uint16_t request_id;
    uint16_t status;
    uint16_t resp_len;
} gw_ctrl_resp_payload_t;
```

Arguments or response bytes follow the respective 8-byte control header.

Common opcodes:

- `GW_OP_HELLO`
- `GW_OP_GET_CAPS`
- `GW_OP_GET_STATUS`
- `GW_OP_STREAM_START`
- `GW_OP_STREAM_STOP`
- `GW_OP_SET_SAMPLE_RATE`
- `GW_OP_SET_PACKET_FRAMES`
- `GW_OP_SET_SAMPLE_FORMAT`
- `GW_OP_SET_CHANNELS`

DAC opcodes:

| Opcode | Args | Meaning |
|---|---|---|
| `GW_OP_DAC_SET_RATE` | `gw_rate_payload_t { uint32_t sample_rate_hz; }` | Set nominal DAC stream rate. |
| `GW_OP_DAC_SET_FORMAT` | `gw_format_payload_t { uint8_t channel_count; uint8_t sample_format; ... }` | Set DAC channel count and sample format. |
| `GW_OP_DAC_WRITE_FRAME` | `uint16_t ch0[, uint16_t ch1]` | Write one immediate DAC frame. |
| `GW_OP_DAC_WRITE_BLOCK` | `gw_dac_data_payload_t + samples` | Write a block through control path. |
| `GW_OP_DAC_STREAM_START` | none | Enable DAC DATA consumption and underrun monitoring. |
| `GW_OP_DAC_STREAM_STOP` | none | Stop DAC DATA consumption. |
| `GW_OP_DAC_FLUSH` | none | Drop/clear pending DAC data; RP2040 backend also writes zero to both channels. |
| `GW_OP_DAC_GET_STATUS` | none | Emit DAC status. |
| `GW_OP_DAC_DTMF_PLAY` | `gw_dtmf_play_payload_t` | Play standard DTMF digits on selected DAC channel mask. |
| `GW_OP_DAC_DTMF_STOP` | none | Stop MCU DTMF generation. |
| `GW_OP_DAC_DTMF_STATUS` | none | Emit MCU DTMF generator status. |
| `GW_OP_GPIO_PERIPH_CONFIG` | `gw_gpio_periph_config_payload_t` | Configure optional MCU GPIO peripherals: MT8870 DTMF decoder pins, CH1817 RI input, and CH1817 OH/OFFHK output. |
| `GW_OP_GPIO_PERIPH_STATUS` | none | Emit MCU GPIO peripheral status. |

## GPIO peripheral config/status

The CM4 server can program optional RP2040 GPIO peripherals at runtime. This avoids reflashing firmware for MT8870 or CH1817 wiring changes.

`gw_gpio_periph_config_payload_t` carries:

- enable flags,
- MT8870 `StQ/Q1/Q2/Q3/Q4` GPIO numbers and polarity,
- CH1817 `RI` GPIO input number and polarity,
- CH1817 `OH/OFFHK` GPIO output number, polarity, and requested drive state,
- MT8870 debounce and event holdoff timings.

Known-good defaults:

| Signal | RP2040 GPIO | Direction | Polarity |
|---|---:|---|---|
| MT8870 `StQ` | 12 | input | active high |
| MT8870 `Q1` | 27 | input | active high |
| MT8870 `Q2` | 26 | input | active high |
| MT8870 `Q3` | 10 | input | active high |
| MT8870 `Q4` | 11 | input | active high |
| CH1817 `RI` | 8 | input from CH1817 | active low |
| CH1817 `OH/OFFHK` | 7 | output to CH1817 | active high |

`gw_gpio_periph_status_payload_t` reports raw/logical MT8870 pins, decoded digit, DTMF event sequence, RI raw/logical state, and OH raw/logical/drive state.

## Status and Events

Telemetry is carried by `STATUS` packets or returned from `GET_STATUS`, not repeated in every data packet.

Examples:

- configured sample rate
- measured sample rate
- lost frames / underruns
- overflow events
- packets sent/received
- CRC errors
- stream active/stopped
- buffer fill
- sample format

Events are emitted on significant changes or errors, such as overflow, stream start/stop, rate change, DAC underrun, flush, or driver fault.

Defined event codes include:

```text
1    overflow
2    counter gap
3    stream started
4    stream stopped
5    rate changed
100  DAC underrun
101  DAC flushed
150  GPIO/MT8870 DTMF digit
151  GPIO changed
200  driver fault
```

## Timing

Default timing reconstruction uses:

```text
stream_epoch + frame_number / sample_rate
```

`TIME_SYNC` packets may be sent on stream start, periodically, or on request:

```c
typedef struct __attribute__((packed)) {
    uint32_t frame_seq;
    uint64_t device_time_us;
} gw_time_sync_payload_t;
```

Timestamps are intentionally not included in every high-rate data packet by default.

## Legacy ADC2 Compatibility

Earlier firmware and server builds supported ADC2:

```text
ADC2 header 32 bytes
sample frame seq/ch0/ch1, 8 bytes
CRC32
```

GWP1 implementations should support ADC2 only as a migration fallback. New devices should use GWP1.
