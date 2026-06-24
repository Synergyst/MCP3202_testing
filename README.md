# CM4 GPIO, MCP3202 Audio Scope, and Caller ID Lab

This project is a Raspberry Pi Compute Module 4 telephony/audio lab. It provides:

- live GPIO monitoring and optional GPIO control
- MCP3202 dual-channel 12-bit ADC sampling
- browser oscilloscope/history display
- WAV capture from the ADC ring buffer
- software Bell 202 Caller ID FSK decoding from ADC audio
- CH1817 DAA ring/off-hook control
- HT9032C/HT9032D hardware Caller ID FSK demodulator monitoring
- configurable Caller ID source selection
- configurable auto-answer after ring detection

The MCP3202 is a 12-bit, dual-channel ADC with an SPI serial interface and two single-ended analog inputs [1]. The HT9032 is a Calling Line Identification physical-layer receiver for Bell 202 and V.23 FSK demodulation [2]. The CH1817 DAA provides the telephone line interface, ring indication, hook switch, and receive audio output [3].

---

## Current default hardware map

### Raspberry Pi 40-pin header summary

```text
Raspberry Pi 40-pin header, top view

  3V3  (1) (2)  5V
 GPIO2 (3) (4)  5V
 GPIO3 (5) (6)  GND
 GPIO4 (7) (8)  GPIO14
   GND (9) (10) GPIO15
GPIO17 (11)(12) GPIO18
GPIO27 (13)(14) GND
GPIO22 (15)(16) GPIO23
  3V3 (17)(18) GPIO24
GPIO10 (19)(20) GND
 GPIO9 (21)(22) GPIO25
GPIO11 (23)(24) GPIO8
   GND (25)(26) GPIO7
 GPIO0 (27)(28) GPIO1
 GPIO5 (29)(30) GND
 GPIO6 (31)(32) GPIO12
GPIO13 (33)(34) GND
GPIO19 (35)(36) GPIO16
GPIO26 (37)(38) GPIO20
   GND (39)(40) GPIO21
```

### MCP3202 ADC wiring

Default ADC wiring uses Raspberry Pi SPI0 plus BCM8 as the MCP3202 chip-select.

```text
Raspberry Pi CM4 / 40-pin header              MCP3202
--------------------------------              -------
PHYS 17 / 3.3V                     ---------> VDD/VREF pin 8
PHYS 25 / GND                      ---------> VSS     pin 4
PHYS 24 / BCM8  / SPI0 CE0_N / CS  ---------> CS/SHDN pin 1
PHYS 23 / BCM11 / SPI0 SCLK        ---------> CLK     pin 7
PHYS 19 / BCM10 / SPI0 MOSI        ---------> DIN     pin 5
PHYS 21 / BCM9  / SPI0 MISO        <--------- DOUT    pin 6
Analog source CH0                  ---------> CH0     pin 2
Analog source CH1                  ---------> CH1     pin 3
```

ASCII block diagram:

```text
                 +------------------------------+
                 | Raspberry Pi CM4 / Linux SPI |
                 |                              |
    BCM8  CE0 ---+------------------------------+---- CS/SHDN
    BCM11 SCLK --+------------------------------+---- CLK
    BCM10 MOSI --+------------------------------+---- DIN
    BCM9  MISO <-+------------------------------+---- DOUT
                 +------------------------------+
                                                      MCP3202
    3.3V ---------------------------------------+---- VDD/VREF
    GND  ---------------------------------------+---- VSS
    audio/test input ---------------------------+---- CH0
    audio/test input ---------------------------+---- CH1
```

The MCP3202 uses `VDD` as the ADC reference, so a channel tied to 3.3 V should read near full-scale, about `4095`, when `VDD/VREF` is also 3.3 V [1]. Communication is SPI-compatible and the MCP3202 supports SPI modes 0,0 and 1,1 [1]. The `CS/SHDN` pin initiates communication when pulled low and must be pulled high between conversions [1].

Important implementation detail: hardware SPI mode automatically restores SPI0 pins BCM9/10/11 to ALT0 before opening `/dev/spidev0.x`. This prevents a previous GPIO/bit-bang test from leaving the SPI pins as plain GPIO and causing all-zero ADC reads.

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

ASCII block diagram:

```text
        Telephone line
        TIP / RING
            |
            v
     +--------------+
     |   CH1817     |
     |     DAA      |
     |              |
     | RI     OFFHK |
     | RCV          |
     +--+-------+---+
        |       |
        |       +-------------------- PHYS 32 / BCM12 output
        |                             low  = on-hook
        |                             high = off-hook
        |
        +---------------------------- PHYS 40 / BCM21 input
                                      active-low ring indication

        RCV audio output ------------ MCP3202/ADC audio input path
```

CH1817 behavior used by the software:

- `OFFHK` low = on-hook.
- `OFFHK` high = off-hook.
- `RI` is asserted low during ringing and high between rings/idle.
- During ring activity, `RI` pulses at the ring frequency, typically around 20 Hz [3].
- `RCV` is the receive audio output and should be AC-coupled into the downstream receive/ADC path [3].

### HT9032C full wiring

Default full wiring assumes you added PHYS 15 for DOUT:

```text
Raspberry Pi CM4 / 40-pin header              HT9032C
--------------------------------              -------
PHYS 36 / BCM16                    ---------> PDWN
PHYS 37 / BCM26                    <--------- CDET
PHYS 15 / BCM22                    <--------- DOUT
PHYS 38 / BCM20                    <--------- DOUTC
optional unused by default         <--------- RDET
GND                                ---------> VSS
3.3V or board supply               ---------> VDD, according to your board design
TIP/RING FSK input network          --------> TIP/RING inputs
```

ASCII block diagram:

```text
                 Telephone line / DAA receive path
                               |
                               v
                        +-------------+
                        |  HT9032C    |
                        | Caller ID   |
                        | receiver    |
                        |             |
       PHYS 36 BCM16 ---+--> PDWN     |
       PHYS 37 BCM26 <--+--- CDET     |
       PHYS 15 BCM22 <--+--- DOUT     |
       PHYS 38 BCM20 <--+--- DOUTC    |
       optional      <--+--- RDET     |
                        +-------------+
```

HT9032 behavior used by the software:

- `PDWN = 1` means power-down.
- `PDWN = 0` means power-up [2].
- `CDET` is an open-drain output that goes low when a valid carrier is present [2].
- `RDET` is an open-drain output that goes low when valid ringing is detected [2].
- `DOUT` outputs the demodulated data stream, including the alternating 1/0 pattern, marking, and data [2].
- `DOUTC` outputs demodulated data after internal validation and does not include the alternating 1/0 pattern [2].

Caller ID Bell 202 facts used by the software:

```text
Logical 1 / Mark  = 1200 Hz
Logical 0 / Space = 2200 Hz
Transmission rate = 1200 bps
Data format       = serial, binary, asynchronous
```

The HT9032 datasheet describes Bell 202 Caller ID using a 1200 Hz mark, 2200 Hz space, and 1200 bps asynchronous serial data [2].

---

## Default reserved pins

When all current drivers are enabled, these pins are owned by chip drivers and hidden from generic GPIO cards:

```text
MCP3202/SPI0:
  PHYS 19 / BCM10 / MOSI
  PHYS 21 / BCM9  / MISO
  PHYS 23 / BCM11 / SCLK
  PHYS 24 / BCM8  / CS

CH1817:
  PHYS 32 / BCM12 / OFFHK
  PHYS 40 / BCM21 / RI

HT9032C:
  PHYS 36 / BCM16 / PDWN
  PHYS 37 / BCM26 / CDET
  PHYS 15 / BCM22 / DOUT
  PHYS 38 / BCM20 / DOUTC
```

---

## Building

```bash
cd /root/mcp-adc/gpio-webui/gpio-webui
make clean
make
```

The executable is:

```text
/root/mcp-adc/gpio-webui/gpio-webui/cm4_gpio_server
```

---

## Running

Recommended current command, matching the local `start.sh` style:

```bash
cd /root/mcp-adc/gpio-webui
/root/mcp-adc/gpio-webui/gpio-webui/cm4_gpio_server \
  --gpio-phys 37,32,36,38,40 \
  --adc-hw-spi \
  --adc-rate 8000 \
  --spi-speed 1800000 \
  --spi-dev /dev/spidev0.0 \
  --adc-cs-bcm 8
```

Then open:

```text
http://<raspberry-pi-ip>:8080/
```

Default listen address is `0.0.0.0`, and default port is `8080`.

---

## Command-line options

```text
Server/config:
  --config PATH        JSON config path, default config.json
  --host ADDR          listen address, default 0.0.0.0
  --port PORT          listen port, default 8080
  -h, --help           show help

GPIO exposure:
  --gpio-only
  --full-gpio
  --adc-disable        disable ADC/graph and expose GPIO-only mode
  --gpio-phys LIST     show only selected physical pins as generic GPIO cards
  --gpio-bcm LIST      show only selected BCM pins as generic GPIO cards

ADC/MCP3202:
  --adc-hw-spi         use Linux spidev hardware SPI
  --adc-bitbang        use direct GPIO bit-banged SPI
  --adc-rate HZ        two-channel frame rate, default 8000
  --adc-history N      ring-buffer samples per channel
  --adc-vref VOLTS     ADC reference voltage for display, default 3.3
  --spi-dev PATH       spidev node, default /dev/spidev0.0
  --spi-speed HZ       SPI clock speed, default 1000000
  --adc-cs-bcm N       MCP3202 chip-select BCM GPIO, default 8
  --adc-clk-bcm N      bit-bang/custom reservation clock GPIO, default 11
  --adc-mosi-bcm N     bit-bang/custom reservation MOSI GPIO, default 10
  --adc-miso-bcm N     bit-bang/custom reservation MISO GPIO, default 9
  --adc-gpio-chip N    gpiochip number for software CS, default 0
```

---

## Web UI sections

### GPIO cards

Generic GPIO cards show:

- physical pin number
- BCM GPIO number
- input/output mode
- live level
- transition state
- measured transition frequency

Pins reserved by ADC, CH1817, or HT9032C are hidden from this generic GPIO section to avoid double-requesting GPIO lines.

### MCP3202 Dual-Channel Historical Micro-Scope

Shows:

- ADC health
- requested and measured sample rate
- latest raw CH0/CH1 readings
- latest voltage estimate using configured `VREF`
- total frames
- dropped reads
- live two-channel history graph

WAV capture supports:

```text
Mono CH0
Mono CH1
Stereo CH0 + CH1
Mono CH0/CH1 mix
```

Optional audio effects are listed by `/api/audio/modules`.

### Software Caller ID FSK Detector

The software detector reads ADC audio and attempts Bell 202 demodulation.

Tuneable fields:

```text
Channel:       CH0, CH1, mix, auto
Mark Hz:       default 1200
Space Hz:      default 2200
Baud:          default 1200
Window ms:     analysis window
Normalize:     on/off
Headroom dB:   normalization headroom
Extra gain dB: post-normalization gain
DC block:      on/off
```

The UI does not overwrite Caller ID tune fields while a field is focused or dirty. Use:

```text
Apply FSK Tune   save current fields
Reload Settings  reload settings from server/JSON
```

### CH1817 DAA

Shows:

```text
RI level
ringing yes/no
RI pulse frequency
on-hook/off-hook state
last error/status
```

Controls:

```text
Go On-Hook
Go Off-Hook
Enable/disable auto-answer
Auto-answer delay in milliseconds
```

Auto-answer behavior:

```text
ring detected -> wait auto_answer_delay_ms -> drive OFFHK high
```

### HT9032C Caller ID Receiver

Shows:

```text
PDWN/power state
CDET carrier state
optional RDET ring state
DOUT logic level
DOUTC logic level
DOUT raw bits and bytes
DOUTC raw bits and bytes
parsed Caller ID data when available
checksum result when available
```

Monitor modes:

```text
both   monitor DOUT and DOUTC
dout   monitor DOUT only
doutc  monitor DOUTC only
```

Preferred FSK source selector:

```text
auto
software_adc
ht9032_dout
ht9032_doutc
```

Runtime `powered` changes are allowed. Pin-map and monitor-mode changes require restart so GPIO lines can be safely released and re-requested.

---

## API endpoints

### General

```text
GET  /api/status
POST /api/config
GET  /api/system/settings
POST /api/system/settings
```

`/api/system/settings` stores the preferred FSK source.

Example:

```json
{
  "fsk_source": "auto"
}
```

### ADC/audio

```text
GET /api/adc?points=1600
GET /api/adc/wav?ms=1000&mode=ch0&codec=pcm16&effects=dc_block
GET /api/audio/modules
```

Example ADC response fields:

```json
{
  "enabled": true,
  "healthy": true,
  "latest_raw": [2259, 4095],
  "latest_volts": [1.82, 3.3],
  "sample_rate_hz": 8000,
  "measured_sample_rate_hz": 7283
}
```

### Software Caller ID

```text
GET  /api/caller-id
GET  /api/caller-id/settings
POST /api/caller-id/settings
```

Example settings POST:

```json
{
  "channel": 0,
  "mark_hz": 1200,
  "space_hz": 2200,
  "baud": 1200,
  "analysis_ms": 5000,
  "normalize": true,
  "normalize_headroom_db": 6,
  "extra_gain_db": 12,
  "dc_block": true
}
```

### CH1817

```text
GET  /api/telephony/ch1817
POST /api/telephony/ch1817/settings
POST /api/telephony/ch1817/offhook
```

Example off-hook request:

```json
{
  "offhook": true
}
```

Example auto-answer settings:

```json
{
  "auto_answer_enabled": true,
  "auto_answer_delay_ms": 1500
}
```

### HT9032C

```text
GET  /api/ht9032
POST /api/ht9032/settings
```

Example settings:

```json
{
  "monitor_mode": "both",
  "powered": true,
  "baud": 1200
}
```

---

## JSON configuration

Settings are saved to the selected JSON config file.

Current shape:

```json
{
  "timeout_ms": 50,
  "fsk_source": "auto",
  "caller_id_settings": {
    "channel": 0,
    "mark_hz": 1200.0,
    "space_hz": 2200.0,
    "baud": 1200.0,
    "analysis_ms": 5000,
    "normalize": true,
    "normalize_headroom_db": 6.0,
    "extra_gain_db": 12.0,
    "dc_block": true
  },
  "caller_id_last": {},
  "ch1817": {
    "enabled": true,
    "offhook_phys": 32,
    "offhook_bcm": 12,
    "ri_phys": 40,
    "ri_bcm": 21,
    "offhook": false,
    "auto_answer_enabled": false,
    "auto_answer_delay_ms": 0
  },
  "ht9032": {
    "enabled": true,
    "pdwn_phys": 36,
    "pdwn_bcm": 16,
    "cdet_phys": 37,
    "cdet_bcm": 26,
    "dout_phys": 15,
    "dout_bcm": 22,
    "doutc_phys": 38,
    "doutc_bcm": 20,
    "rdet_phys": 0,
    "rdet_bcm": -1,
    "pdwn_control": true,
    "powered": true,
    "active_low_cdet": true,
    "active_low_rdet": true,
    "monitor_mode": "both",
    "baud": 1200
  },
  "pins": {}
}
```

---

## Pin validation rules

The server validates chip pin assignments before use.

Rules:

- Only GPIO-capable 40-pin-header physical pins are accepted.
- Enabled chip signals must not share the same physical pin.
- Reserved chip pins are hidden from generic GPIO control.
- CH1817 pin mapping changes require restart.
- HT9032C pin mapping and monitor-mode changes require restart.
- Runtime HT9032C power changes are allowed.
- Runtime CH1817 off-hook and auto-answer changes are allowed.

GPIO-capable physical pins:

```text
3, 5, 7, 8, 10, 11, 12, 13, 15, 16, 18,
19, 21, 22, 23, 24, 26, 27, 28, 29, 31,
32, 33, 35, 36, 37, 38, 40
```

If an illegal configuration is submitted through the API/UI, the server returns an error and help text. The browser shows that as a help/alert dialog.

---

## Caller ID data flow

```text
                    +-------------------------+
                    | Telephone line TIP/RING |
                    +-----------+-------------+
                                |
                                v
                         +-------------+
                         |   CH1817    |
                         | DAA module  |
                         +--+-------+--+
                            |       |
                   RI ------+       +------ RCV audio
                   |                       |
                   v                       v
          CH1817 ring state         MCP3202 CH0/CH1
          auto-answer logic              ADC samples
                   |                       |
                   v                       v
             OFFHK control       software Bell 202 FSK
                                           |
                                           v
                                     Caller ID parser

Alternative / parallel hardware demodulator path:

       line/receive FSK audio ---> HT9032C ---> DOUT/DOUTC ---> async 1200 bps parser
```

---

## Troubleshooting

### ADC reads 0/0

If CH1 is tied to 3.3 V and both channels still read zero, the digital SPI path is the likely issue, not the analog input.

Check:

```bash
raspi-gpio get 8-11
```

Expected for hardware SPI after startup:

```text
GPIO 8:  CS, either GPIO output if software-CS is used, or ALT0 if controller CE is used
GPIO 9:  ALT0 SPI0_MISO
GPIO 10: ALT0 SPI0_MOSI
GPIO 11: ALT0 SPI0_SCLK
```

The server now restores BCM9/10/11 to SPI0 ALT0 automatically when hardware SPI mode is used. BCM8 is intentionally left as a GPIO output when `--adc-cs-bcm 8` is used, because the application drives MCP3202 CS directly.

Useful direct test:

```bash
/root/mcp-adc/mcp3202_spidev_test --dev /dev/spidev0.0 --speed 1800000 --samples 5
```

Useful bit-bang sanity test on the same wires:

```bash
/root/mcp-adc/mcp3202_bitbang_test --cs 8 --clk 11 --din 10 --dout 9 --samples 5 --delay-us 2
```

### Caller ID FSK is weak or noisy

- Confirm ADC channel selection.
- Try CH0, CH1, mix, and auto.
- Increase analysis window.
- Enable normalization.
- Try modest extra gain.
- Compare software ADC decoding against HT9032C `CDET`, `DOUT`, and `DOUTC`.
- Use `DOUT` for raw visibility and `DOUTC` for cleaner validated data.

### HT9032C shows no carrier

Check:

- `powered` is enabled, or PDWN is otherwise held low.
- `CDET` polarity matches the hardware. Default is active-low.
- The HT9032C clock/crystal is running.
- The input network is connected to the FSK source you intend to test.
- Caller ID usually appears between the first and second ring.

### CH1817 ring status does not change

Check:

- PHYS 40 / BCM21 is connected to RI.
- RI is pulled/biased correctly for your board.
- Ring signal reaches TIP/RING.
- The Web UI timeout does not affect the CH1817 driver; CH1817 has its own worker.

---

## Implementation notes

- MCP3202 hardware SPI mode uses 3-byte transfers compatible with the datasheet's 8-bit segment examples.
- The ADC ring buffer stores CH0 and CH1 raw 12-bit samples.
- ADC graph decimation uses min/max buckets so peaks remain visible.
- WAV export converts raw 12-bit ADC samples to signed 16-bit PCM.
- Caller ID software decoding scans several bit phases and can auto-select CH0, CH1, or mix.
- HT9032C digital decoding reads async serial as 1200 bps, 8 data bits, LSB first, one start bit, one stop bit.
- Caller ID parsing handles SDMF `0x04` and MDMF `0x80` message framing.
- CH1817 and HT9032C drivers own their GPIO lines directly through `libgpiod`.
- Generic GPIO cards do not request reserved driver pins.

---

## Datasheet-backed facts used by this project

- MCP3202: 12-bit ADC, two single-ended inputs, SPI interface, VDD/VREF reference, supports SPI modes 0,0 and 1,1 [1].
- MCP3202: `CS/SHDN` low initiates communication; `CS/SHDN` high ends conversion and places the device in standby; it must be high between conversions [1].
- MCP3202: the analog input code is proportional to `VIN / VDD`, so a 3.3 V input with 3.3 V reference should be near full scale [1].
- HT9032: Bell 202 Caller ID uses mark 1200 Hz, space 2200 Hz, 1200 bps asynchronous serial data [2].
- HT9032: `CDET` goes low when valid carrier is present; `DOUT` includes preamble/marking/data; `DOUTC` omits the alternating preamble pattern after validation [2].
- CH1817: `OFFHK` low is on-hook and high is off-hook; `RI` is active-low and pulses at the ring frequency during ringing [3].
- CH1817: `RCV` is the receive audio output and must be AC-coupled [3].
