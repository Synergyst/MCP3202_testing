#pragma once

#include <array>
#include <cstdint>
#include <string>

class MCP4922 {
public:
    enum class Channel : uint8_t { A = 0, B = 1 };

    struct ChannelConfig {
        // true = VREF input buffered, false = unbuffered.
        bool buffered_vref = false;
        // false = 2x output gain, true = 1x output gain (matches MCP4922 GA bit).
        bool gain_1x = true;
        // false = software shutdown for this DAC channel, true = active output.
        bool active = true;
        double vref = 3.3;
    };

    struct Config {
        bool enabled = false;
        // false = Linux spidev hardware SPI, true = direct GPIO bit-banged SPI.
        bool bitbang = false;

        // Hardware SPI settings. Set cs_bcm to -1 to let the SPI controller drive CE0/CE1.
        // MCP4922 supports SPI mode 0,0 and 1,1; mode 0 is the default.
        std::string device = "/dev/spidev0.1";
        uint32_t speed_hz = 10000000;
        uint8_t mode = 0;
        uint8_t bits_per_word = 8;

        // Software/custom wiring settings. Default CS is Raspberry Pi SPI0 CE1: physical 26 / BCM7.
        int clk_bcm = 11;
        int mosi_bcm = 10;
        int cs_bcm = 7;

        // Optional control pins. Use -1 when tied externally or not connected.
        // LDAC low latches input registers to outputs. If controlled, writes keep LDAC high and pulse low.
        int ldac_bcm = -1;
        // Hardware SHDN low disables both channels. If controlled, driver keeps it high while open.
        int shdn_bcm = -1;

        std::string gpio_chip = "0";
        ChannelConfig channel[2];
    };

    MCP4922();
    explicit MCP4922(Config config);
    ~MCP4922();

    MCP4922(const MCP4922&) = delete;
    MCP4922& operator=(const MCP4922&) = delete;

    void open();
    void close();
    bool isOpen() const;

    void writeRaw(Channel channel, uint16_t code);
    void writeRawBoth(uint16_t code_a, uint16_t code_b, bool latch_after = true);
    void shutdown(Channel channel);
    void wake(Channel channel);
    void latchOutputs();
    void setHardwareShutdown(bool enabled);
    uint16_t voltsToRaw(Channel channel, double volts) const;
    double rawToVolts(Channel channel, uint16_t raw) const;
    Config config() const { return config_; }

private:
    void openHardwareSpi();
    void openBitBang();
    void configureSpi();
    void writeCommand(uint16_t command);
    void writeCommandHardwareSpi(uint16_t command);
    void writeCommandBitBang(uint16_t command);
    uint16_t buildCommand(Channel channel, uint16_t code, const ChannelConfig& cfg) const;
    void setSoftwareCs(bool selected);

    void gpioSetOutput(int bcm);
    void gpioWrite(int bcm, bool high);
    void gpioDelay() const;
    void validateBcm(int bcm, const char* name) const;
    void openGpioMap(const std::string& purpose);
    void restoreSpi0Alt0PinsIfNeeded();

    Config config_;
    int fd_ = -1;
    int gpiomem_fd_ = -1;
    volatile uint32_t* gpio_ = nullptr;
};
