# CH1817LM / CM4 GPIO WebUI / RP2040 ADC-DAC Phone System

This repository contains the Raspberry Pi CM4 GPIO/WebUI server, shared GWP1 protocol headers, and RP2040 firmware used to build a working CH1817LM-based phone interface with ADC scope, DAC output, DTMF generation, MT8870 DTMF decode validation, and telephony RI/OH control.

## Top-level layout

- `gpio-webui/` — CM4/Linux server, embedded WebUI, GPIO control, ADC/DAC abstractions, telephony stack, audio upload/playback, filter profiles, and self-tests.
- `mcu-adc/` — RP2040 firmware for MCP3202 ADC streaming, MCP4922 DAC output, DTMF generation, MT8870 GPIO decode, and optional CH1817 RI/OH offload.
- `protocol/` — shared GWP1 host/MCU protocol headers and documentation.
- `config.json` — main CM4 server runtime/persisted configuration.
- `start.sh` — server startup helper.

## Working phone-system architecture

The CM4 server remains the system coordinator. The RP2040 can offload timing-sensitive and extra-GPIO tasks.

Main data/control paths:

```text
MCP3202 ADC -> RP2040 -> GWP1 USB CDC -> CM4 WebUI/telephony DSP
CM4 WebUI/API -> GWP1 USB CDC -> RP2040 -> MCP4922 DAC / DTMF generation
MT8870 Q/StQ -> RP2040 GPIO -> GWP1 status -> CM4 DTMF decoder history/validation
CH1817 RI -> RP2040 GPIO input or CM4 GPIO input -> CM4 telephony state
CM4 telephony off-hook request -> CM4 GPIO or RP2040 GPIO output -> CH1817 OH/OFFHK
```

## Current known-good MCU peripheral defaults

These defaults match the validated bench wiring and are also configurable from `config.json` / WebUI:

| Function | RP2040 GPIO | Direction | Default polarity |
|---|---:|---|---|
| MT8870 `StQ` / valid strobe | GP12 | input | active high |
| MT8870 `Q1` | GP27 | input | active high |
| MT8870 `Q2` | GP26 | input | active high |
| MT8870 `Q3` | GP10 | input | active high |
| MT8870 `Q4` | GP11 | input | active high |
| CH1817 `RI` | GP8 | input from CH1817 | active low |
| CH1817 `OH` / `OFFHK` | GP7 | output to CH1817 | active high |

Important: the MT8870 analog DTMF input source is not assumed by software. It can be fed from any properly conditioned DTMF source.

## Canonical config section

RP2040 peripheral mapping is stored under top-level `mcu_peripherals`, not under the CM4 physical-header `pins` section.

```json
{
  "mcu_peripherals": {
    "enabled": true,
    "dtmf_decoder": {
      "enabled": true,
      "source": "mcu_mt8870",
      "pins": { "stq": 12, "q1": 27, "q2": 26, "q3": 10, "q4": 11 },
      "polarity": { "stq_active_high": true, "q_active_high": true },
      "debounce_ms": 2,
      "event_holdoff_ms": 25,
      "history_limit": 64,
      "validation": { "enabled": true, "raw_poll_hz": 20 }
    },
    "ch1817_signals": {
      "enabled": true,
      "ri": { "source": "mcu", "gpio": 8, "active_high": false },
      "oh": { "source": "mcu", "gpio": 7, "active_high": true }
    }
  }
}
```

If the section is missing, the CM4 server writes canonical defaults on startup while preserving unrelated config keys.

## Build

CM4 server:

```bash
cd /root/mcp-adc/gpio-webui/gpio-webui
make -j4
```

RP2040 firmware:

```bash
cd /root/mcp-adc/gpio-webui/mcu-adc/build
make -j4
```

## Flash RP2040

Put the RP2040 into BOOTSEL mode, then copy the UF2:

```bash
mkdir -p /mnt/rpi-rp2
mount /dev/sda1 /mnt/rpi-rp2
cp /root/mcp-adc/gpio-webui/mcu-adc/build/mcu_adc.uf2 /mnt/rpi-rp2/
sync
umount /mnt/rpi-rp2
```

## WebUI validation quick path

1. Start the CM4 server.
2. Open the WebUI and go to the DAC / DTMF area.
3. Confirm MCU peripheral config uses the known-good pins above.
4. Click `Apply MCU Peripheral Config`.
5. Enter expected DTMF sequence `1234567890*#` and click `Start Validation`.
6. Dial the sequence.
7. Confirm decoded digits and raw bits match.
8. Use `Clear History` between tests.

## Main APIs added for MCU peripherals

- `GET /api/mcu/peripherals/status`
- `POST /api/mcu/peripherals/config`
- `GET /api/dac/dtmf/decoder/status`
- `POST /api/dac/dtmf/decoder/history/clear`

Existing telephony APIs continue to be used for CH1817 off-hook and state:

- `GET /api/telephony/ch1817`
- `POST /api/telephony/ch1817/offhook`
- `GET /api/telephony/state`

## Notes

- `pins` in `config.json` is for CM4 40-pin physical GPIO state only.
- `mcu_peripherals` is for RP2040 GPIO mapping and source selection.
- DTMF history is runtime-only by default and is user-clearable from the WebUI.
- Build outputs and object files are generated in-tree by the existing Makefiles.
