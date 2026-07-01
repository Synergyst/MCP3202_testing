# Audio Upload, Playback, Filters, and DTMF Notes

## Upload/playback support

The CM4 WebUI supports uploaded audio playback to the MCP4922 DAC path.

Supported upload format in this implementation:

- RIFF/WAVE container.
- PCM format tag 1.
- 16-bit little-endian samples.
- Mono or stereo.
- Sample rate up to 100 kHz.
- Upload size up to 50 MB.

Uploaded file bytes are stored under `uploads/audio/` relative to the active `config.json` directory. Metadata is stored in `config.json` under `audio_uploads`; the original filename is display-only and the stored filename is generated from a server-side upload id.

Playback routing modes:

- `ch0`: mono mix/source to DAC A, DAC B held at midpoint 2048.
- `ch1`: mono mix/source to DAC B, DAC A held at midpoint 2048.
- `mono_both`: mono mix/source to DAC A and DAC B.
- `stereo`: stereo left to DAC A and right to DAC B; mono sources duplicate to both channels.

For native CM4 SPI playback, frames are paced from userspace with `sleep_until()`. RP2040/GWP1 playback uses DAC data blocks and should be preferred for smoother continuous playback.

## Filter profiles

Filter profiles are persisted in `config.json` under `filter_profiles` and are managed through the WebUI or `/api/audio/filter-profile` API.

Important playback profiles:

- `dac.playback.ch0`
- `dac.playback.ch1`
- `dac.playback.stereo`

Other task-specific profiles include:

- `adc.graph.ch0`
- `adc.graph.ch1`
- `adc.wav.ch0`
- `adc.wav.ch1`
- `adc.wav.mix`
- `adc.wav.stereo`
- `fsk.decoder`
- `line_state.detector`
- `telephony.diagnostics`
- `dac.dtmf`

Invalid or unavailable filters are rejected before being persisted.

## DTMF generation

MCU DTMF generation is implemented in RP2040 firmware and is selected through the DAC DTMF API/WebUI controls. The generator uses:

- Standard DTMF row/column frequencies.
- Fixed 8 kHz synthesis rate for accurate phase increments.
- 32-bit phase accumulators.
- Sample-count based tone/gap timing.
- 5 ms attack/release ramping to reduce clicks.
- DAC midpoint 2048 as silence for inactive channels and tone gaps.

DTMF generation is independent from MT8870 DTMF detection. The MT8870 decoder is exposed as an MCU GPIO peripheral and reports raw `StQ/Q1-Q4`, decoded digits, and a runtime last-N history through the WebUI/API.

Known-good MT8870 GPIO mapping:

| MT8870 signal | RP2040 GPIO |
|---|---:|
| `StQ` | GP12 |
| `Q1` | GP27 |
| `Q2` | GP26 |
| `Q3` | GP10 |
| `Q4` | GP11 |

The MT8870 analog input source is not assumed. It can be any properly conditioned DTMF source.

## CH1817 MCU RI/OH offload

The MCU peripheral path can also handle CH1817 sideband signals when configured in `mcu_peripherals`:

| CH1817 signal | RP2040 GPIO | Direction | Polarity |
|---|---:|---|---|
| `RI` | GP8 | input from CH1817 | active low |
| `OH` / `OFFHK` | GP7 | output to CH1817 | active high |

The CM4 telephony stack uses MCU RI/OH only when the corresponding source is set to `mcu`; otherwise the existing CM4 GPIO path remains available.

## Self-tests

The CM4 build includes `audio_pipeline_selftest`, which checks:

- Raw ADC to PCM16 mapping.
- PCM16 back to raw ADC midpoint behavior.
- WAV encode/probe/load roundtrip for PCM16 stereo.
- Empty filter-chain no-op behavior.
- Basic filter catalog validation.
