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

The GWP1 DTMF protocol structs were not changed; status `remaining_ms` is derived from remaining synthesis samples.

## Self-tests

The CM4 build includes `audio_pipeline_selftest`, which checks:

- Raw ADC to PCM16 mapping.
- PCM16 back to raw ADC midpoint behavior.
- WAV encode/probe/load roundtrip for PCM16 stereo.
- Empty filter-chain no-op behavior.
- Basic filter catalog validation.
