#include "MCP3202.hpp"

#include <fcntl.h>
#include <linux/spi/spidev.h>
#include <linux/gpio.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace {
constexpr size_t GPIO_BLOCK_SIZE = 4096;
constexpr int GPFSEL0 = 0;
constexpr int GPSET0 = 7;
constexpr int GPCLR0 = 10;
constexpr int GPLEV0 = 13;

std::string errnoMessage(const std::string& prefix) {
    return prefix + ": " + std::strerror(errno);
}

void setGpioFunction(volatile uint32_t* gpio, int bcm, uint32_t function) {
    const int reg = GPFSEL0 + (bcm / 10);
    const int shift = (bcm % 10) * 3;
    uint32_t v = gpio[reg];
    v &= ~(0x7u << shift);
    v |= ((function & 0x7u) << shift);
    gpio[reg] = v;
}

void restoreSpi0Alt0PinsIfNeeded(const MCP3202::Config& config) {
    // Bit-bang mode intentionally owns the pins as GPIO. Hardware SPI mode needs
    // BCM9/10/11 back in ALT0 after any previous GPIO/bit-bang test run left
    // them as ordinary GPIO. Otherwise /dev/spidev0.x transfers can silently read
    // all zeroes even though the ADC and wiring are fine.
    if (config.device.find("spidev0.") == std::string::npos) return;

    int mem_fd = ::open("/dev/gpiomem", O_RDWR | O_SYNC | O_CLOEXEC);
    if (mem_fd < 0) mem_fd = ::open("/dev/mem", O_RDWR | O_SYNC | O_CLOEXEC);
    if (mem_fd < 0) throw std::runtime_error(errnoMessage("opening /dev/gpiomem or /dev/mem to restore SPI0 pin mux"));

    volatile uint32_t* gpio = static_cast<volatile uint32_t*>(::mmap(nullptr, GPIO_BLOCK_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED, mem_fd, 0));
    int saved_errno = errno;
    ::close(mem_fd);
    if (gpio == MAP_FAILED) {
        errno = saved_errno;
        throw std::runtime_error(errnoMessage("mapping GPIO registers to restore SPI0 pin mux"));
    }

    try {
        constexpr uint32_t ALT0 = 4;
        setGpioFunction(gpio, 9, ALT0);   // SPI0 MISO
        setGpioFunction(gpio, 10, ALT0);  // SPI0 MOSI
        setGpioFunction(gpio, 11, ALT0);  // SPI0 SCLK
        if (config.cs_bcm < 0) {
            if (config.device.find("spidev0.1") != std::string::npos) setGpioFunction(gpio, 7, ALT0); // CE1
            else setGpioFunction(gpio, 8, ALT0); // CE0
        }
    } catch (...) {
        ::munmap(const_cast<uint32_t*>(gpio), GPIO_BLOCK_SIZE);
        throw;
    }
    ::munmap(const_cast<uint32_t*>(gpio), GPIO_BLOCK_SIZE);
}
}

MCP3202::MCP3202() : config_(Config()) {}

MCP3202::MCP3202(Config config) : config_(std::move(config)) {}

MCP3202::~MCP3202() {
    close();
}

void MCP3202::open() {
    if (isOpen()) return;
    if (config_.bitbang) openBitBang();
    else openHardwareSpi();
}

void MCP3202::openHardwareSpi() {
    if (fd_ >= 0) return;

    restoreSpi0Alt0PinsIfNeeded(config_);

    fd_ = ::open(config_.device.c_str(), O_RDWR | O_CLOEXEC);
    if (fd_ < 0) {
        throw std::runtime_error(errnoMessage("opening " + config_.device));
    }

    try {
        configureSpi();

        if (config_.cs_bcm >= 0) {
            validateBcm(config_.cs_bcm, "cs_bcm");
            std::string chip_path = "/dev/gpiochip" + config_.gpio_chip;
            int chip_fd = ::open(chip_path.c_str(), O_RDONLY | O_CLOEXEC);
            if (chip_fd < 0) {
                throw std::runtime_error(errnoMessage("opening " + chip_path + " for MCP3202 software CS"));
            }

            gpiohandle_request req{};
            req.lineoffsets[0] = static_cast<__u32>(config_.cs_bcm);
            req.flags = GPIOHANDLE_REQUEST_OUTPUT;
            req.default_values[0] = 1; // MCP3202 CS idle high
            req.lines = 1;
            std::snprintf(req.consumer_label, sizeof(req.consumer_label), "mcp3202_cs");

            if (::ioctl(chip_fd, GPIO_GET_LINEHANDLE_IOCTL, &req) < 0) {
                int saved = errno;
                ::close(chip_fd);
                errno = saved;
                throw std::runtime_error(errnoMessage("requesting BCM" + std::to_string(config_.cs_bcm) + " as MCP3202 CS"));
            }
            ::close(chip_fd);
            cs_fd_ = req.fd;
        }
    } catch (...) {
        close();
        throw;
    }
}

void MCP3202::openBitBang() {
    validateBcm(config_.clk_bcm, "clk_bcm");
    validateBcm(config_.mosi_bcm, "mosi_bcm");
    validateBcm(config_.miso_bcm, "miso_bcm");
    validateBcm(config_.cs_bcm, "cs_bcm");

    gpiomem_fd_ = ::open("/dev/gpiomem", O_RDWR | O_SYNC | O_CLOEXEC);
    if (gpiomem_fd_ < 0) {
        // Root can usually use /dev/mem on systems where /dev/gpiomem is absent.
        gpiomem_fd_ = ::open("/dev/mem", O_RDWR | O_SYNC | O_CLOEXEC);
        if (gpiomem_fd_ < 0) {
            throw std::runtime_error(errnoMessage("opening /dev/gpiomem or /dev/mem for MCP3202 bitbang"));
        }
    }

    gpio_ = static_cast<volatile uint32_t*>(::mmap(nullptr, GPIO_BLOCK_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED, gpiomem_fd_, 0));
    if (gpio_ == MAP_FAILED) {
        gpio_ = nullptr;
        close();
        throw std::runtime_error(errnoMessage("mapping GPIO registers for MCP3202 bitbang"));
    }

    gpioSetOutput(config_.clk_bcm);
    gpioSetOutput(config_.mosi_bcm);
    gpioSetOutput(config_.cs_bcm);
    gpioSetInput(config_.miso_bcm);
    gpioWrite(config_.cs_bcm, true);
    gpioWrite(config_.clk_bcm, false);
    gpioWrite(config_.mosi_bcm, false);
}

void MCP3202::close() {
    if (cs_fd_ >= 0) {
        try { setSoftwareCs(false); } catch (...) {}
        ::close(cs_fd_);
        cs_fd_ = -1;
    }
    if (fd_ >= 0) {
        ::close(fd_);
        fd_ = -1;
    }
    if (gpio_) {
        try {
            gpioWrite(config_.cs_bcm, true);
            gpioWrite(config_.clk_bcm, false);
            gpioWrite(config_.mosi_bcm, false);
        } catch (...) {}
        ::munmap(const_cast<uint32_t*>(gpio_), GPIO_BLOCK_SIZE);
        gpio_ = nullptr;
    }
    if (gpiomem_fd_ >= 0) {
        ::close(gpiomem_fd_);
        gpiomem_fd_ = -1;
    }
}

bool MCP3202::isOpen() const {
    return config_.bitbang ? (gpio_ != nullptr) : (fd_ >= 0);
}

void MCP3202::configureSpi() {
    uint8_t mode = config_.mode;
    uint8_t bits = config_.bits_per_word;
    uint32_t speed = config_.speed_hz;

    if (::ioctl(fd_, SPI_IOC_WR_MODE, &mode) < 0) {
        throw std::runtime_error(errnoMessage("setting SPI mode"));
    }
    if (::ioctl(fd_, SPI_IOC_WR_BITS_PER_WORD, &bits) < 0) {
        throw std::runtime_error(errnoMessage("setting SPI bits-per-word"));
    }
    if (::ioctl(fd_, SPI_IOC_WR_MAX_SPEED_HZ, &speed) < 0) {
        throw std::runtime_error(errnoMessage("setting SPI speed"));
    }
}

void MCP3202::setSoftwareCs(bool selected) {
    if (cs_fd_ < 0) return;
    gpiohandle_data data{};
    data.values[0] = selected ? 0 : 1; // active-low chip select
    if (::ioctl(cs_fd_, GPIOHANDLE_SET_LINE_VALUES_IOCTL, &data) < 0) {
        throw std::runtime_error(errnoMessage("setting MCP3202 software CS"));
    }
}

uint16_t MCP3202::readChannel(int channel) {
    if (channel != 0 && channel != 1) {
        throw std::invalid_argument("MCP3202 channel must be 0 or 1");
    }
    if (!isOpen()) open();
    return config_.bitbang ? readChannelBitBang(channel) : readChannelHardwareSpi(channel);
}

uint16_t MCP3202::readChannelHardwareSpi(int channel) {
    // MCP3202 hardware-SPI framing using 8-bit segments as shown in the datasheet:
    // byte0: seven leading zeros then Start=1
    // byte1: SGL/DIFF=1, ODD/SIGN=channel, MSBF=1, then don't-care zeros
    // byte2: don't-care clocks to receive B7..B0
    // RX byte1 low nibble contains B11..B8, RX byte2 contains B7..B0.
    uint8_t tx[3] = {
        0x01,
        static_cast<uint8_t>(0xA0 | (channel ? 0x40 : 0x00)),
        0x00
    };
    uint8_t rx[3] = { 0, 0, 0 };

    spi_ioc_transfer tr{};
    tr.tx_buf = reinterpret_cast<unsigned long>(tx);
    tr.rx_buf = reinterpret_cast<unsigned long>(rx);
    tr.len = sizeof(tx);
    tr.speed_hz = config_.speed_hz;
    tr.bits_per_word = config_.bits_per_word;

    setSoftwareCs(true);
    int rc = ::ioctl(fd_, SPI_IOC_MESSAGE(1), &tr);
    setSoftwareCs(false);

    if (rc < 1) {
        throw std::runtime_error(errnoMessage("MCP3202 SPI transfer"));
    }

    return static_cast<uint16_t>(((rx[1] & 0x0F) << 8) | rx[2]);
}

uint16_t MCP3202::readChannelBitBang(int channel) {
    // Match the datasheet's 24-clock / 8-bit-segment SPI mode-0 framing:
    // TX byte0 = 00000001 (seven leading zeros + Start)
    // TX byte1 = SGL=1, ODD=channel, MSBF=1, then don't-cares
    // TX byte2 = don't-cares. RX byte1 low nibble is B11..B8, RX byte2 is B7..B0.
    const uint8_t tx[3] = {
        0x01,
        static_cast<uint8_t>(0xA0 | (channel ? 0x40 : 0x00)),
        0x00
    };
    uint8_t rx[3] = {0, 0, 0};

    gpioWrite(config_.cs_bcm, true);
    gpioWrite(config_.clk_bcm, false);
    gpioWrite(config_.mosi_bcm, false);
    gpioDelay();
    gpioWrite(config_.cs_bcm, false);
    gpioDelay();

    for (int byte = 0; byte < 3; ++byte) {
        for (int bit = 7; bit >= 0; --bit) {
            const bool din = ((tx[byte] >> bit) & 0x01) != 0;
            gpioWrite(config_.mosi_bcm, din);
            gpioDelay();
            gpioWrite(config_.clk_bcm, true);
            gpioDelay();
            gpioWrite(config_.clk_bcm, false);
            gpioDelay();
            rx[byte] = static_cast<uint8_t>((rx[byte] << 1) | (gpioRead(config_.miso_bcm) ? 1 : 0));
        }
    }

    gpioWrite(config_.cs_bcm, true);
    gpioWrite(config_.mosi_bcm, false);
    return static_cast<uint16_t>(((rx[1] & 0x0F) << 8) | rx[2]);
}

double MCP3202::rawToVolts(uint16_t raw, double vref) const {
    return (static_cast<double>(raw) * vref) / 4095.0;
}

void MCP3202::validateBcm(int bcm, const char* name) const {
    if (bcm < 0 || bcm > 53) {
        throw std::invalid_argument(std::string(name) + " must be a BCM GPIO number from 0 to 53");
    }
}

void MCP3202::gpioSetOutput(int bcm) {
    const int reg = GPFSEL0 + (bcm / 10);
    const int shift = (bcm % 10) * 3;
    uint32_t v = gpio_[reg];
    v &= ~(0x7u << shift);
    v |= (0x1u << shift);
    gpio_[reg] = v;
}

void MCP3202::gpioSetInput(int bcm) {
    const int reg = GPFSEL0 + (bcm / 10);
    const int shift = (bcm % 10) * 3;
    uint32_t v = gpio_[reg];
    v &= ~(0x7u << shift);
    gpio_[reg] = v;
}

void MCP3202::gpioWrite(int bcm, bool high) {
    gpio_[(high ? GPSET0 : GPCLR0) + (bcm / 32)] = (1u << (bcm % 32));
}

bool MCP3202::gpioRead(int bcm) const {
    return (gpio_[GPLEV0 + (bcm / 32)] & (1u << (bcm % 32))) != 0;
}

void MCP3202::gpioDelay() const {
    // Keeps bit-banged SPI conservative/reliable on Linux while still fast enough for UI capture.
    // Hardware SPI should be used for precise 8 kHz dual-channel audio acquisition.
    for (volatile int i = 0; i < 20; ++i) {}
}
