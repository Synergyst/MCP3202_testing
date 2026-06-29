#include "MCP4922.hpp"

#include <fcntl.h>
#include <linux/spi/spidev.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cmath>
#include <cstring>
#include <stdexcept>
#include <string>
#include <utility>

namespace {
constexpr size_t GPIO_BLOCK_SIZE = 4096;
constexpr int GPFSEL0 = 0;
constexpr int GPSET0 = 7;
constexpr int GPCLR0 = 10;

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
}

MCP4922::MCP4922() : config_(Config()) {}
MCP4922::MCP4922(Config config) : config_(std::move(config)) {}
MCP4922::~MCP4922() { close(); }

void MCP4922::open() {
    if (isOpen()) return;
    if (!config_.enabled) return;
    if (config_.bitbang) openBitBang();
    else openHardwareSpi();
}

void MCP4922::openHardwareSpi() {
    if (fd_ >= 0) return;
    restoreSpi0Alt0PinsIfNeeded();
    fd_ = ::open(config_.device.c_str(), O_RDWR | O_CLOEXEC);
    if (fd_ < 0) throw std::runtime_error(errnoMessage("opening " + config_.device));
    try {
        configureSpi();
        if (config_.cs_bcm >= 0 || config_.ldac_bcm >= 0 || config_.shdn_bcm >= 0) {
            openGpioMap("MCP4922 GPIO control");
            if (config_.cs_bcm >= 0) { validateBcm(config_.cs_bcm, "cs_bcm"); gpioSetOutput(config_.cs_bcm); gpioWrite(config_.cs_bcm, true); }
            if (config_.ldac_bcm >= 0) { validateBcm(config_.ldac_bcm, "ldac_bcm"); gpioSetOutput(config_.ldac_bcm); gpioWrite(config_.ldac_bcm, true); }
            if (config_.shdn_bcm >= 0) { validateBcm(config_.shdn_bcm, "shdn_bcm"); gpioSetOutput(config_.shdn_bcm); gpioWrite(config_.shdn_bcm, true); }
        }
    } catch (...) { close(); throw; }
}

void MCP4922::openBitBang() {
    validateBcm(config_.clk_bcm, "clk_bcm");
    validateBcm(config_.mosi_bcm, "mosi_bcm");
    validateBcm(config_.cs_bcm, "cs_bcm");
    if (config_.ldac_bcm >= 0) validateBcm(config_.ldac_bcm, "ldac_bcm");
    if (config_.shdn_bcm >= 0) validateBcm(config_.shdn_bcm, "shdn_bcm");
    openGpioMap("MCP4922 bitbang");
    gpioSetOutput(config_.clk_bcm);
    gpioSetOutput(config_.mosi_bcm);
    gpioSetOutput(config_.cs_bcm);
    if (config_.ldac_bcm >= 0) gpioSetOutput(config_.ldac_bcm);
    if (config_.shdn_bcm >= 0) gpioSetOutput(config_.shdn_bcm);
    gpioWrite(config_.cs_bcm, true);
    gpioWrite(config_.clk_bcm, false);
    gpioWrite(config_.mosi_bcm, false);
    if (config_.ldac_bcm >= 0) gpioWrite(config_.ldac_bcm, true);
    if (config_.shdn_bcm >= 0) gpioWrite(config_.shdn_bcm, true);
}

void MCP4922::close() {
    if (gpio_) {
        try {
            if (config_.cs_bcm >= 0) gpioWrite(config_.cs_bcm, true);
            if (config_.clk_bcm >= 0) gpioWrite(config_.clk_bcm, false);
            if (config_.mosi_bcm >= 0) gpioWrite(config_.mosi_bcm, false);
            if (config_.ldac_bcm >= 0) gpioWrite(config_.ldac_bcm, true);
        } catch (...) {}
        ::munmap(const_cast<uint32_t*>(gpio_), GPIO_BLOCK_SIZE);
        gpio_ = nullptr;
    }
    if (fd_ >= 0) { ::close(fd_); fd_ = -1; }
    if (gpiomem_fd_ >= 0) { ::close(gpiomem_fd_); gpiomem_fd_ = -1; }
}

bool MCP4922::isOpen() const { return config_.bitbang ? (gpio_ != nullptr) : (fd_ >= 0); }

void MCP4922::configureSpi() {
    uint8_t mode = config_.mode;
    uint8_t bits = config_.bits_per_word;
    uint32_t speed = config_.speed_hz;
    if (::ioctl(fd_, SPI_IOC_WR_MODE, &mode) < 0) throw std::runtime_error(errnoMessage("setting MCP4922 SPI mode"));
    if (::ioctl(fd_, SPI_IOC_WR_BITS_PER_WORD, &bits) < 0) throw std::runtime_error(errnoMessage("setting MCP4922 SPI bits-per-word"));
    if (::ioctl(fd_, SPI_IOC_WR_MAX_SPEED_HZ, &speed) < 0) throw std::runtime_error(errnoMessage("setting MCP4922 SPI speed"));
}

uint16_t MCP4922::buildCommand(Channel channel, uint16_t code, const ChannelConfig& cfg) const {
    code &= 0x0FFFu;
    uint16_t cmd = code;
    if (channel == Channel::B) cmd |= 0x8000u;       // A/B: 1 = DACB, 0 = DACA
    if (cfg.buffered_vref) cmd |= 0x4000u;           // BUF: 1 = buffered VREF
    if (cfg.gain_1x) cmd |= 0x2000u;                 // GA: 1 = 1x, 0 = 2x
    if (cfg.active) cmd |= 0x1000u;                  // SHDN: 1 = active, 0 = shutdown selected channel
    return cmd;
}

void MCP4922::writeRaw(Channel channel, uint16_t code) {
    if (!config_.enabled) return;
    if (!isOpen()) open();
    writeCommand(buildCommand(channel, code, config_.channel[static_cast<int>(channel)]));
    if (config_.ldac_bcm >= 0) latchOutputs();
}

void MCP4922::writeRawBoth(uint16_t code_a, uint16_t code_b, bool latch_after) {
    if (!config_.enabled) return;
    if (!isOpen()) open();
    writeCommand(buildCommand(Channel::A, code_a, config_.channel[0]));
    writeCommand(buildCommand(Channel::B, code_b, config_.channel[1]));
    if (latch_after && config_.ldac_bcm >= 0) latchOutputs();
}

void MCP4922::shutdown(Channel channel) {
    if (!config_.enabled) return;
    ChannelConfig cfg = config_.channel[static_cast<int>(channel)];
    cfg.active = false;
    if (!isOpen()) open();
    writeCommand(buildCommand(channel, 0, cfg));
    if (config_.ldac_bcm >= 0) latchOutputs();
}

void MCP4922::wake(Channel channel) {
    if (!config_.enabled) return;
    config_.channel[static_cast<int>(channel)].active = true;
    writeRaw(channel, 0);
}

void MCP4922::writeCommand(uint16_t command) {
    if (config_.bitbang) writeCommandBitBang(command);
    else writeCommandHardwareSpi(command);
}

void MCP4922::writeCommandHardwareSpi(uint16_t command) {
    uint8_t tx[2] = { static_cast<uint8_t>(command >> 8), static_cast<uint8_t>(command & 0xFF) };
    spi_ioc_transfer tr{};
    tr.tx_buf = reinterpret_cast<unsigned long>(tx);
    tr.len = sizeof(tx);
    tr.speed_hz = config_.speed_hz;
    tr.bits_per_word = config_.bits_per_word;
    setSoftwareCs(true);
    int rc = ::ioctl(fd_, SPI_IOC_MESSAGE(1), &tr);
    setSoftwareCs(false);
    if (rc < 1) throw std::runtime_error(errnoMessage("MCP4922 SPI transfer"));
}

void MCP4922::writeCommandBitBang(uint16_t command) {
    gpioWrite(config_.cs_bcm, true);
    gpioWrite(config_.clk_bcm, false);
    gpioDelay();
    gpioWrite(config_.cs_bcm, false);
    gpioDelay();
    for (int bit = 15; bit >= 0; --bit) {
        gpioWrite(config_.mosi_bcm, ((command >> bit) & 1u) != 0);
        gpioDelay();
        gpioWrite(config_.clk_bcm, true);
        gpioDelay();
        gpioWrite(config_.clk_bcm, false);
        gpioDelay();
    }
    gpioWrite(config_.cs_bcm, true);
    gpioWrite(config_.mosi_bcm, false);
}

void MCP4922::setSoftwareCs(bool selected) {
    if (config_.cs_bcm >= 0 && gpio_) gpioWrite(config_.cs_bcm, !selected);
}

void MCP4922::latchOutputs() {
    if (config_.ldac_bcm < 0) return;
    if (!gpio_) openGpioMap("MCP4922 LDAC");
    gpioWrite(config_.ldac_bcm, false);
    gpioDelay();
    gpioWrite(config_.ldac_bcm, true);
}

void MCP4922::setHardwareShutdown(bool enabled) {
    if (config_.shdn_bcm < 0) return;
    if (!gpio_) openGpioMap("MCP4922 SHDN");
    gpioWrite(config_.shdn_bcm, !enabled); // SHDN is active low.
}

uint16_t MCP4922::voltsToRaw(Channel channel, double volts) const {
    const ChannelConfig& cfg = config_.channel[static_cast<int>(channel)];
    const double gain = cfg.gain_1x ? 1.0 : 2.0;
    const double full_scale = std::max(1e-12, cfg.vref * gain);
    const double clamped = std::max(0.0, std::min(volts, full_scale * 4095.0 / 4096.0));
    return static_cast<uint16_t>(std::lround((clamped * 4096.0) / full_scale)) & 0x0FFFu;
}

double MCP4922::rawToVolts(Channel channel, uint16_t raw) const {
    const ChannelConfig& cfg = config_.channel[static_cast<int>(channel)];
    const double gain = cfg.gain_1x ? 1.0 : 2.0;
    return (static_cast<double>(raw & 0x0FFFu) * cfg.vref * gain) / 4096.0;
}

void MCP4922::validateBcm(int bcm, const char* name) const {
    if (bcm < 0 || bcm > 53) throw std::invalid_argument(std::string(name) + " must be a BCM GPIO number from 0 to 53");
}

void MCP4922::openGpioMap(const std::string& purpose) {
    if (gpio_) return;
    gpiomem_fd_ = ::open("/dev/gpiomem", O_RDWR | O_SYNC | O_CLOEXEC);
    if (gpiomem_fd_ < 0) {
        gpiomem_fd_ = ::open("/dev/mem", O_RDWR | O_SYNC | O_CLOEXEC);
        if (gpiomem_fd_ < 0) throw std::runtime_error(errnoMessage("opening /dev/gpiomem or /dev/mem for " + purpose));
    }
    gpio_ = static_cast<volatile uint32_t*>(::mmap(nullptr, GPIO_BLOCK_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED, gpiomem_fd_, 0));
    if (gpio_ == MAP_FAILED) { gpio_ = nullptr; throw std::runtime_error(errnoMessage("mapping GPIO registers for " + purpose)); }
}

void MCP4922::restoreSpi0Alt0PinsIfNeeded() {
    if (config_.bitbang || config_.device.find("spidev0.") == std::string::npos) return;
    openGpioMap("MCP4922 SPI0 pin mux restore");
    constexpr uint32_t ALT0 = 4;
    setGpioFunction(gpio_, 10, ALT0); // SPI0 MOSI
    setGpioFunction(gpio_, 11, ALT0); // SPI0 SCLK
    if (config_.cs_bcm < 0) {
        if (config_.device.find("spidev0.1") != std::string::npos) setGpioFunction(gpio_, 7, ALT0);
        else setGpioFunction(gpio_, 8, ALT0);
    }
}

void MCP4922::gpioSetOutput(int bcm) {
    const int reg = GPFSEL0 + (bcm / 10);
    const int shift = (bcm % 10) * 3;
    uint32_t v = gpio_[reg];
    v &= ~(0x7u << shift);
    v |= (0x1u << shift);
    gpio_[reg] = v;
}

void MCP4922::gpioWrite(int bcm, bool high) {
    gpio_[(high ? GPSET0 : GPCLR0) + (bcm / 32)] = (1u << (bcm % 32));
}

void MCP4922::gpioDelay() const {
    for (volatile int i = 0; i < 20; ++i) {}
}
