#pragma once

#include <array>
#include <cstdint>
#include <string>

class MCP3202 {
public:
    struct Config {
        // false = Linux spidev hardware SPI, true = direct GPIO bit-banged SPI.
        bool bitbang = false;

        // Hardware SPI settings. Set cs_bcm to -1 to let the SPI controller drive CE0/CE1.
        std::string device = "/dev/spidev0.0";
        uint32_t speed_hz = 1000000;
        uint8_t mode = 0;
        uint8_t bits_per_word = 8;

        // Software/custom wiring settings. The same cs_bcm is used for hardware software-CS
        // and bit-banged CS. Default CS is SPI0 CE0: physical pin 24 / BCM8.
        int clk_bcm = 11;
        int mosi_bcm = 10;
        int miso_bcm = 9;
        int cs_bcm = 8;

        // Used only for hardware SPI software-CS fallback.
        std::string gpio_chip = "0";
    };

    MCP3202();
    explicit MCP3202(Config config);
    ~MCP3202();

    MCP3202(const MCP3202&) = delete;
    MCP3202& operator=(const MCP3202&) = delete;

    void open();
    void close();
    bool isOpen() const;

    uint16_t readChannel(int channel);
    std::array<uint16_t, 2> readBothChannels();
    double rawToVolts(uint16_t raw, double vref = 3.3) const;

private:
    void openHardwareSpi();
    void openBitBang();
    void configureSpi();
    void setSoftwareCs(bool selected);
    uint16_t readChannelHardwareSpi(int channel);
    uint16_t readChannelBitBang(int channel);

    void gpioSetOutput(int bcm);
    void gpioSetInput(int bcm);
    void gpioWrite(int bcm, bool high);
    bool gpioRead(int bcm) const;
    void gpioDelay() const;
    void validateBcm(int bcm, const char* name) const;

    Config config_;
    int fd_ = -1;
    int cs_fd_ = -1;
    int gpiomem_fd_ = -1;
    volatile uint32_t* gpio_ = nullptr;
};
