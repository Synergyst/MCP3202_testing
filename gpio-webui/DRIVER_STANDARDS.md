# ADC/DAC Driver Implementation Standards

This project uses small, device-specific C++ driver modules behind explicit configuration structs. New ADC/DAC drivers should follow the style used by `MCP3202` and `MCP4922`.

## File layout

For a device named `XYZ1234`, add:

```text
gpio-webui/XYZ1234.hpp
gpio-webui/XYZ1234.cpp
```

The header should expose:

- `class XYZ1234`
- nested `struct Config`
- `open()` / `close()` / `isOpen()`
- typed read/write methods
- conversion helpers, for example `rawToVolts()` / `voltsToRaw()`

## Configuration struct

Every driver config should be self-contained and have safe defaults:

- `enabled=false` for optional peripherals.
- `bitbang=false` when hardware SPI is expected.
- spidev path, speed, mode, bits-per-word.
- BCM GPIO numbers for software CS or bitbang pins.
- optional device-specific control pins set to `-1` when not wired.
- per-channel analog parameters such as VREF, gain, buffering, active/shutdown state.

Do not open hardware from the config constructor. The driver should be lazy and only open on first I/O unless the owner explicitly calls `open()`.

## SPI behavior

Driver code must be derived from the device datasheet and documented in comments near the transfer code.

For hardware SPI:

- Use Linux `spidev`.
- Configure mode, bits-per-word, and speed in `configureSpi()`.
- Support `cs_bcm=-1` to allow the kernel SPI controller to drive CE.
- Support a fast direct-register software CS path when `cs_bcm >= 0`.
- Restore SPI0 pin mux when using `/dev/spidev0.x`, because previous GPIO/bitbang runs may leave pins in GPIO mode.

For bitbang SPI:

- Use `/dev/gpiomem` or `/dev/mem` mapping.
- Validate BCM ranges.
- Keep timing conservative and deterministic enough for setup/hold requirements.
- Leave outputs in safe idle states in `close()`.

## Error handling

- Throw `std::runtime_error` with a clear prefix and `strerror(errno)` for OS failures.
- Throw `std::invalid_argument` for bad channels/pins/ranges.
- Destructors must call `close()` and must not throw.

## Optional peripherals

Optional devices such as DACs must be disabled by default and must not require hardware to be attached during normal server startup.

If enabled through CLI, persist settings to `config.json` so the CLI flags may be removed on later runs.

## Config persistence pattern

For optional drivers:

1. Load saved `dac_*` or `adc_*` keys before CLI parsing.
2. CLI flags override loaded settings.
3. If any relevant CLI flag was supplied, write the resulting settings back to config.
4. Reserve GPIO pins only when the device is enabled.
5. Prefer lazy opening so startup does not fail if outputs are not used immediately.

## Documentation expectations

Each driver must document:

- wiring defaults
- SPI mode and command framing
- supported channels/resolution
- control pins and whether they may be tied externally
- config keys and CLI flags
- known limitations and safety notes
