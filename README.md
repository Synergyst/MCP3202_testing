# CM4 GPIO, MCP3202/RP2040 Audio Scope, and Telephony Lab

This project is a Raspberry Pi Compute Module 4 telephony/audio lab. It provides:

- **Live GPIO monitoring and control**: Real-time tracking of the 40-pin header.
- **Dual-channel 12-bit ADC sampling**: Support for MCP3202 (SPI) or RP2040 (USB CDC).
- **Browser Oscilloscope**: Live history display with min/max decimation for audio peaks.
- **WAV capture**: Direct export of ADC ring buffer data with optional DSP effects.
- **Line State Detection**: DSP-based detection of telephony tones (Dial Tone, Busy, Ringback, etc.) with RI corroboration.
- **Telephony Coordination**: A state machine managing the lifecycle of a call, including auto-answer (after rings or Caller ID) and auto-hangup.
- **Caller ID Decoding**: Software Bell 202 FSK decoding from ADC audio and monitoring of HT9032C hardware demodulators.
- **DAA Control**: CH1817-based ring detection and off-hook control.
- **Telephony Diagnostics**: Calibration tools for noise floors, tone scanning, and hardware validation.

The MCP3202 is a 12-bit, dual-channel ADC with an SPI serial interface [1]. The RP2040 provides a high-speed USB-based ADC alternative. The HT9032 is a Calling Line Identification physical-layer receiver for Bell 202 and V.23 FSK demodulation [2]. The CH1817 DAA provides the telephone line interface, ring indication, hook switch, and receive audio output [3].

---

## Current default hardware map

### Numbering convention: physical vs BCM vs WiringPi/Pi4J

This project is deliberately documented with **two** pin-numbering systems only:

```text
PHYS = physical 40-pin header position, 1 through 40
BCM  = raw Broadcom GPIO number used by Linux, libgpiod, raspi-gpio, and this program's *_bcm options
```

Use this translation table when comparing different diagrams:

```text
Physical header pin -> raw BCM GPIO -> common WiringPi/Pi4J GPIO# label

PHYS  3 -> BCM2  -> WiringPi/Pi4J GPIO# 8
PHYS  5 -> BCM3  -> WiringPi/Pi4J GPIO# 9
PHYS  7 -> BCM4  -> WiringPi/Pi4J GPIO# 7
PHYS  8 -> BCM14 -> WiringPi/Pi4J GPIO# 15
PHYS 10 -> BCM15 -> WiringPi/Pi4J GPIO# 16
PHYS 11 -> BCM17 -> WiringPi/Pi4J GPIO# 0
PHYS 12 -> BCM18 -> WiringPi/Pi4J GPIO# 1
PHYS 13 -> BCM27 -> WiringPi/Pi4J GPIO# 2
PHYS 15 -> BCM22 -> WiringPi/Pi4J GPIO# 3
PHYS 16 -> BCM23 -> WiringPi/Pi4J GPIO# 4
PHYS 18 -> BCM24 -> WiringPi/Pi4J GPIO# 5
PHYS 19 -> BCM10 -> WiringPi/Pi4J GPIO# 12
PHYS 21 -> BCM9  -> WiringPi/Pi4J GPIO# 13
PHYS 22 -> BCM25 -> WiringPi/Pi4J GPIO# 6
PHYS 23 -> BCM11 -> WiringPi/Pi4J GPIO# 14
PHYS 24 -> BCM8  -> WiringPi/Pi4J GPIO# 10
PHYS 26 -> BCM7  -> WiringPi/Pi4J GPIO# 11
PHYS 27 -> BCM0  -> WiringPi/Pi4J GPIO# 30
PHYS 28 -> BCM1  -> WiringPi/Pi4J GPIO# 31
PHYS 29 -> BCM5  -> WiringPi/Pi4J GPIO# 21
PHYS 31 -> BCM6  -> WiringPi/Pi4J GPIO# 22
PHYS 32 -> BCM12 -> WiringPi/Pi4J GPIO# 26
PHYS 33 -> BCM13 -> WiringPi/Pi4J GPIO# 23
PHYS 35 -> BCM19 -> WiringPi/Pi4J GPIO# 24
PHYS 36 -> BCM16 -> WiringPi/Pi4J GPIO# 27
PHYS 37 -> BCM26 -> WiringPi/Pi4J GPIO# 25
PHYS 38 -> BCM20 -> WiringPi/Pi4J GPIO# 28
PHYS 40 -> BCM21 -> WiringPi/Pi4J GPIO# 29
```

### Raspberry Pi 40-pin header summary

```text
Raspberry Pi 40-pin header, top view, raw BCM labels

  3V3  (1) (2)  5V
 BCM2  (3) (4)  5V
 BCM3  (5) (6)  GND
 BCM4  (7) (8)  BCM14
   GND (9) (10) BCM15
 BCM17 (11)(12) BCM18
 BCM27 (13)(14) GND
 BCM22 (15)(16) BCM23
  3V3 (17)(18) BCM24
 BCM10(19)(20) GND
 BCM9 (21)(22) BCM25
 BCM11(23)(24) BCM8
   GND (25)(26) BCM7
 BCM0 (27)(28) BCM1
 BCM5 (29)(30) GND
 BCM6 (31)(32) BCM12
 BCM13(33)(34) GND
 BCM19(35)(36) BCM16
 BCM26(37)(38) BCM20
   GND (39)(40) BCM21
```

### Project default pin assignments with alternate labels

```text
Signal                 PHYS pin   raw BCM   WiringPi/Pi4J label   Program setting
---------------------------------------------------------------------------------
MCP3202 CS/SHDN        24         BCM8      GPIO# 10              --adc-cs-bcm 8
MCP3202 CLK/SCLK       23         BCM11     GPIO# 14              --adc-clk-bcm 11
MCP3202 DIN/MOSI       19         BCM10     GPIO# 12              --adc-mosi-bcm 10
MCP3202 DOUT/MISO      21         BCM9      GPIO# 13              --adc-miso-bcm 9

CH1817 OFFHK           32         BCM12     GPIO# 26              ch1817.offhook_phys=32
CH1817 RI              40         BCM21     GPIO# 29              ch1817.ri_phys=40

HT9032C PDWN           36         BCM16     GPIO# 27              ht9032.pdwn_phys=36
HT9032C CDET           37         BCM26     GPIO# 25              ht9032.cdet_phys=37
HT9032C DOUT           15         BCM22     GPIO# 3               ht9032.dout_phys=15
HT9032C DOUTC          38         BCM20     GPIO# 28              ht9032.doutc_phys=38
HT9032C RDET optional  unused     unused    unused                ht9032.rdet_phys=0
```

### MCP3202 ADC wiring

Default ADC wiring uses Raspberry Pi SPI0 plus BCM8 as the MCP3202 chip-select.

```text
Raspberry Pi CM4 / 40-pin header              MCP3202
--------------------------------              ------
PHYS 17 / 3.3V                     ---------> VDD/VREF pin 8
PHYS 25 / GND                      ---------> VSS     pin 4
PHYS 24 / BCM8  / SPI0 CE0_N / CS  ---------> CS/SHDN pin 1
PHYS 23 / BCM11 / SPI0 SCLK        ---------> CLK     pin 7
PHYS 19 / BCM10 / SPI0 MOSI        ---------> DIN     pin 5
PHYS 21 / BCM9  / SPI0 MISO        <--------- DOUT    pin 6
Analog source CH0                  ---------> CH0     pin 2
Analog source CH1                  ---------> CH1     pin 3
```

### CH1817 DAA wiring

```text
Raspberry Pi CM4 / 40-pin header              CH1817
--------------------------------              ------
PHYS 32 / BCM12                    ---------> OFFHK
PHYS 40 / BCM21                    <--------- RI
GND                                ---------> GND
5V or board supply                 ---------> VCC, according to your board design
CH1817 RCV                         ---------> ADC/audio conditioning path
TIP/RING                           <--------> telephone line interface
```

### HT9032C full wiring

```text
Raspberry Pi CM4 / 40-pin header              HT9032C
--------------------------------              ------
PHYS 36 / BCM16                    ---------> PDWN
PHYS 37 / BCM26                    <--------- CDET
PHYS 15 / BCM22                    <--------- DOUT
PHYS 38 / BCM20                    <--------- DOUTC
optional unused by default         <--------- RDET
GND                                ---------> VSS
3.3V or board supply               ---------> VDD, according to your board design
TIP/RING FSK input network          --------> TIP/RING inputs
```

---

## Building

```bash
cd /root/mcp-adc/gpio-webui/gpio-webui
make clean
make
```

The executable is: `/root/mcp-adc/gpio-webui/gpio-webui/cm4_gpio_server`

---

## Running

Recommended current command:

```bash
cd /root/mcp-adc/gpio-webui
/root/mcp-adc/gpio-webui/gpio-webui/cm4_gpio_server \\
  --gpio-phys 37,32,36,38,40 \\
  --adc-hw-spi \\
  --adc-rate 8000 \\
  --spi-speed 1800000 \\
  --spi-dev /dev/spidev0.0 \\
  --adc-cs-bcm 8
```

Then open: `http://<raspberry-pi-ip>:8080/`

---

## Command-line options

### Server/config
- `--config PATH`: JSON config path, default `config.json`
- `--host ADDR`: Listen address, default `0.0.0.0`
- `--port PORT`: Listen port, default `8080`
- `-h, --help`: Show help

### GPIO exposure
- `--gpio-only` / `--full-gpio`: Disable ADC/graph and expose all 40-pin-header GPIOs.
- `--adc-disable`: Alias for `--gpio-only`.
- `--gpio-phys LIST`: Show only selected physical pins as generic GPIO cards.
- `--gpio-bcm LIST`: Show only selected BCM pins as generic GPIO cards.

### ADC / MCP3202 / RP2040
- `--adc-source SOURCE`: ADC source: `mcp3202-spidev` (default) or `rp2040`.
- `--adc-rp2040-dev PATH`: RP2040 USB device path, default `/dev/ttyACM0`.
- `--adc-hw-spi`: Use Linux spidev hardware SPI (default).
- `--adc-bitbang`: Use direct GPIO bit-banged SPI.
- `--adc-rate HZ`: Two-channel frame rate, default `8000`.
- `--adc-history N`: Ring-buffer samples per channel.
- `--adc-history-ms MS`: Ring-buffer history duration in milliseconds, default `30000`.
- `--adc-max-buffer-mb MB`: Cap combined ADC ring-buffer RAM usage, default `64`.
- `--adc-vref VOLTS`: ADC reference voltage for display, default `3.3`.
- `--adc-realtime`: Run ADC sampler thread with `SCHED_FIFO` priority.
- `--adc-rt-priority N`: `SCHED_FIFO` priority (1..99), default `10`.
- `--adc-cpu N`: Pin ADC sampler thread to CPU N (-1 disables affinity).
- `--spi-dev PATH`: spidev node, default `/dev/spidev0.0`.
- `--spi-speed HZ`: SPI clock speed, default `1000000`.
- `--adc-cs-bcm N`: MCP3202 chip-select BCM GPIO, default `8`.
- `--adc-clk-bcm N`: Bit-bang/custom reservation clock GPIO, default `11`.
- `--adc-mosi-bcm N`: Bit-bang/custom reservation MOSI GPIO, default `10`.
- `--adc-miso-bcm N`: Bit-bang/custom reservation MISO GPIO, default `9`.
- `--adc-gpio-chip N`: gpiochip number for software CS, default `0`.

---

## Web UI sections

The interface is organized into tabs: **Scope / Audio**, **Caller ID**, **Telephony**, and **GPIO**.

### Scope / Audio
- **ADC Scope**: Live two-channel history graph with support for Raw, Filtered, or Overlay views.
- **ADC Configuration**: Switch between RP2040 and MCP3202, and adjust sample rates.
- **WAV Capture**: Download or preview ring-buffer history as 16-bit PCM WAV.
- **Audio Effects**: Real-time application of DSP effects (DC block, Hum Notch, Voice AGC, Bandpass, etc.).

### Caller ID
- **FSK Detector**: Monitors ADC audio for Bell 202 demodulation.
- **Tuning**: Configurable Mark/Space frequencies, Baud rate, and Analysis window.
- **Raw Data**: View raw FSK bits and bytes along with the best-confidence decode.

### Telephony
- **Unified Telephony State**: High-level view of the system state (e.g., `Ringing`, `OffHook`, `CallerIdPending`).
- **Coordinator Controls**: Configure auto-answer (delay, min rings, wait for CID) and auto-hangup logic.
- **CH1817 DAA**: Monitor RI level and frequency; control on-hook/off-hook state.
- **Line State Detector**: Real-time DSP identification of line tones (Dial Tone, Busy, etc.) with region-based profiles (e.g., NANP).
- **Calibration & Diagnostics**: 
    - Measure idle noise floor to recommend RMS thresholds.
    - Scan for current line tones.
    - Capture disconnect profiles.
    - Export system health and validation checklists.

### GPIO
- **Generic GPIO cards**: Live monitoring and control of physical header pins (excluding those reserved by chip drivers).

---

## API endpoints

### General
- `GET  /api/status`: Current state of all exposed GPIOs.
- `POST /api/config`: Update GPIO modes, states, or server timeout.
- `GET  /api/system/settings`: Get preferred FSK source.
- `POST /api/system/settings`: Set preferred FSK source.

### ADC / Audio
- `GET  /api/adc?points=N&view=V&effects=E`: Get scope data.
- `POST /api/adc/config`: Update ADC source, device, or rate.
- `GET  /api/adc/wav?ms=N&mode=M&codec=C&effects=E`: Download WAV.
- `GET  /api/audio/modules`: List available audio DSP effects.

### Telephony
- `GET  /api/telephony/state`: Unified coordinator snapshot.
- `POST /api/telephony/state/settings`: Update coordinator settings.
- `GET  /api/telephony/line-state`: Current line state and confidence.
- `POST /api/telephony/line-state/settings`: Update line state settings (region, etc.).
- `GET  /api/telephony/ch1817`: DAA status.
- `POST /api/telephony/ch1817/settings`: Update CH1817 settings.
- `POST /api/telephony/ch1817/offhook`: Drive off-hook/on-hook.
- `GET  /api/telephony/hardware-check`: System health check.
- `POST /api/telephony/calibration/rcv-noise-floor`: Measure noise floor.
- `POST /api/telephony/calibration/tone-scan`: Scan for current tones.
- `POST /api/telephony/calibration/apply-line-thresholds`: Apply recommended RMS levels.
- `GET  /api/telephony/diagnostics/export`: Export full diagnostics.

### Caller ID
- `GET  /api/caller-id`: Latest decoded Caller ID data.
- `GET  /api/caller-id/settings`: FSK detector settings.
- `POST /api/caller-id/settings`: Update FSK detector settings.

---

## JSON configuration

Settings are saved to the selected JSON config file.

```json
{
  \"timeout_ms\": 50,
  \"fsk_source\": \"auto\",
  \"caller_id_settings\": {
    \"channel\": 0,
    \"mark_hz\": 1200.0,
    \"space_hz\": 2200.0,
    \"baud\": 1200.0,
    \"analysis_ms\": 5000,
    \"normalize\": true,
    \"normalize_headroom_db\": 6.0,
    \"extra_gain_db\": 12.0,
    \"dc_block\": true
  },
  \"ch1817\": {
    \"enabled\": true,
    \"offhook_phys\": 32,
    \"ri_phys\": 40,
    \"offhook\": false,
    \"auto_answer_enabled\": false,
    \"auto_answer_delay_ms\": 0
  },
  \"ht9032\": {
    \"enabled\": true,
    \"pdwn_phys\": 36,
    \"cdet_phys\": 37,
    \"dout_phys\": 15,
    \"doutc_phys\": 38,
    \"powered\": true,
    \"monitor_mode\": \"both\",
    \"baud\": 1200
  },
  \"pins\": {}
}
```

---

## Troubleshooting

### ADC reads 0/0
If using MCP3202, verify SPI wiring and check `raspi-gpio get 8-11`. Ensure the pins are in `ALT0` mode. The server now restores these automatically when hardware SPI is used.

### Line State / Telephony issues
- Use the **Calibration** tools in the Telephony tab to measure the idle noise floor.
- Apply the recommended `Silence RMS` and `Min RMS` thresholds.
- Use the **Tone Scan** to verify that the line is delivering expected frequencies for your region.
- Check the **Hardware Check** for warnings about ADC health or RI polarity.

### Caller ID is noisy
- Confirm ADC channel selection (try CH0, CH1, or Mix).
- Enable **Normalization** and apply modest **Extra Gain**.
- Compare software ADC decoding against the HT9032C `CDET` and `DOUTC` lines.
