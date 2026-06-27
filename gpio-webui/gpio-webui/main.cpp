#include <iostream>
#include <map>
#include <memory>
#include <thread>
#include <set>
#include <string>
#include <vector>
#include <sstream>
#include <stdexcept>
#include <algorithm>
#include "PinConfig.hpp"
#include "ConfigManager.hpp"
#include "GpioManager.hpp"
#include "WebServer.hpp"
#include "SystemContext.hpp"
#include "AdcSampler.hpp"
#include "CallerIdDetector.hpp"
#include "Ch1817Driver.hpp"
#include "HeaderPins.hpp"

namespace {
const std::vector<std::pair<int, int>>& headerPinMap() {
    // Raspberry Pi 40-pin header GPIO-capable pins: physical pin -> BCM GPIO.
    static const std::vector<std::pair<int, int>> pins = {
        {3, 2}, {5, 3}, {7, 4}, {8, 14}, {10, 15},
        {11, 17}, {12, 18}, {13, 27}, {15, 22}, {16, 23}, {18, 24},
        {19, 10}, {21, 9}, {22, 25}, {23, 11}, {24, 8}, {26, 7},
        {27, 0}, {28, 1}, {29, 5}, {31, 6}, {32, 12}, {33, 13},
        {35, 19}, {36, 16}, {37, 26}, {38, 20}, {40, 21}
    };
    return pins;
}

void printUsage(const char* argv0, std::ostream& os) {
    os << "Usage: " << argv0 << " [options]\n\n"
       << "Modes:\n"
       << "  (default)                         ADC graph mode + GPIO control except reserved SPI/ADC pins\n"
       << "  --gpio-only | --full-gpio          Disable ADC/graph and expose all 40-pin-header GPIOs\n"
       << "  --adc-disable                      Alias for --gpio-only\n"
       << "  --gpio-phys LIST                   Expose only listed physical pins as GPIO controls\n"
       << "  --gpio-bcm LIST                    Expose only listed BCM GPIOs as GPIO controls\n"
       << "                                     --gpio-phys and --gpio-bcm are mutually exclusive\n\n"
       << "LIST format:\n"
       << "  Comma-separated and/or space-separated integers, e.g.\n"
       << "    --gpio-phys 32,36,38,40\n"
       << "    --gpio-bcm 12,16,20,21\n"
       << "  If using spaces, quote the list or repeat shell-safe comma form.\n\n"
       << "ADC options:\n"
       << "  --adc-source SOURCE               ADC source: 'mcp3202-spidev' (default) or 'rp2040'\n"
       << "  --adc-rp2040-dev PATH             RP2040 USB device path (default /dev/ttyACM0)\n"
       << "  --adc-bitbang                      Use GPIO bit-banged SPI for MCP3202\n"
       << "  --adc-hw-spi                       Use Linux spidev hardware SPI (default)\n"
       << "  --adc-rate HZ                      ADC two-channel frame rate (default 8000)\n"
       << "  --adc-history N                    Samples of history per channel (overrides duration defaults)\n"
       << "  --adc-history-ms MS                Ring-buffer history duration in milliseconds (default 30000)\n"
       << "  --adc-max-buffer-mb MB             Cap ADC ring-buffer RAM usage for both channels (default 64)\n"
       << "  --adc-vref VOLTS                   ADC reference voltage for display (default 3.3)\n"
       << "  --adc-realtime                     Run ADC sampler thread with modest SCHED_FIFO priority\n"
       << "  --adc-rt-priority N                SCHED_FIFO priority for --adc-realtime (1..99, default 10)\n"
       << "  --adc-cpu N                        Pin ADC sampler thread to CPU N (-1 disables affinity)\n"
       << "  --spi-dev PATH                     spidev node (default /dev/spidev0.0)\n"
       << "  --spi-speed HZ                     SPI clock speed (default 1000000)\n"
       << "  --adc-cs-bcm N                     ADC chip-select BCM GPIO, or -1 for controller CE\n"
       << "  --adc-clk-bcm N                    ADC clock BCM GPIO (bitbang/custom reservation; default 11)\n"
       << "  --adc-mosi-bcm N                   ADC MOSI/DIN BCM GPIO (default 10)\n"
       << "  --adc-miso-bcm N                   ADC MISO/DOUT BCM GPIO (default 9)\n"
       << "  --adc-gpio-chip N                  gpiochip number string for software CS (default 0)\n\n"
       << "Server/config options:\n"
       << "  --config PATH                      JSON config path (default config.json)\n"
       << "  --host ADDR                        Listen address (default 0.0.0.0)\n"
       << "  --port PORT                        Listen port (default 8080)\n"
       << "  -h, --help                         Show this help\n\n"
       << "Notes:\n"
       << "  In ADC graph mode, all configured SPI/ADC pins are hidden and protected from GPIO control.\n"
       << "  --gpio-phys/--gpio-bcm only limit which GPIO cards are shown; they do not disable ADC.\n"
       << "  Combine --gpio-only with --gpio-phys or --gpio-bcm for custom GPIO-only mode.\n";
}

int parseIntStrict(const std::string& s, const std::string& what) {
    size_t pos = 0;
    int v = 0;
    try {
        v = std::stoi(s, &pos, 10);
    } catch (...) {
        throw std::runtime_error("Invalid integer for " + what + ": '" + s + "'");
    }
    if (pos != s.size()) {
        throw std::runtime_error("Invalid integer for " + what + ": '" + s + "'");
    }
    return v;
}

uint32_t parseUInt32Strict(const std::string& s, const std::string& what) {
    int v = parseIntStrict(s, what);
    if (v <= 0) throw std::runtime_error(what + " must be positive");
    return static_cast<uint32_t>(v);
}

std::vector<int> parseIntList(std::string list, const std::string& what) {
    std::replace(list.begin(), list.end(), ',', ' ');
    std::istringstream iss(list);
    std::vector<int> out;
    std::string tok;
    while (iss >> tok) out.push_back(parseIntStrict(tok, what));
    if (out.empty()) throw std::runtime_error(what + " list is empty");
    return out;
}

bool hasValue(int i, int argc, char* argv[]) {
    return i + 1 < argc && std::string(argv[i + 1]).rfind("--", 0) != 0;
}

std::string requireValue(int& i, int argc, char* argv[], const std::string& opt) {
    if (!hasValue(i, argc, argv)) {
        throw std::runtime_error("Missing value for " + opt);
    }
    return argv[++i];
}

std::map<int, std::shared_ptr<PinState>> buildFullHeaderRegistry() {
    std::map<int, std::shared_ptr<PinState>> registry;
    for (const auto& [phys, bcm] : headerPinMap()) {
        registry[phys] = std::make_shared<PinState>(phys, bcm);
    }
    return registry;
}

std::map<int, int> bcmToPhysicalMap() {
    std::map<int, int> m;
    for (const auto& [phys, bcm] : headerPinMap()) m[bcm] = phys;
    return m;
}

std::map<int, std::shared_ptr<PinState>> buildPhysicalRegistry(const std::vector<int>& physical_pins) {
    auto all = buildFullHeaderRegistry();
    std::map<int, std::shared_ptr<PinState>> out;
    for (int phys : physical_pins) {
        auto it = all.find(phys);
        if (it == all.end()) {
            throw std::runtime_error("Physical pin " + std::to_string(phys) + " is not a GPIO-capable 40-pin-header pin");
        }
        out[phys] = std::make_shared<PinState>(phys, it->second->bcm_pin);
    }
    return out;
}

std::map<int, std::shared_ptr<PinState>> buildBcmRegistry(const std::vector<int>& bcm_pins) {
    auto bcm_to_phys = bcmToPhysicalMap();
    std::map<int, std::shared_ptr<PinState>> out;
    for (int bcm : bcm_pins) {
        auto it = bcm_to_phys.find(bcm);
        if (it == bcm_to_phys.end()) {
            throw std::runtime_error("BCM GPIO " + std::to_string(bcm) + " is not present as a GPIO-capable 40-pin-header pin");
        }
        int phys = it->second;
        out[phys] = std::make_shared<PinState>(phys, bcm);
    }
    return out;
}

std::set<int> configuredAdcPins(const AdcSampler::Config& adc_config) {
    std::set<int> pins;
    if (!adc_config.enabled) return pins;

    if (adc_config.adc_source == "rp2040") {
        return pins; // USB source, no GPIO pins reserved
    }

    // These are the configured SPI signal pins. In hardware-SPI mode the defaults
    // are the Pi SPI0 pins; in bitbang mode they are the custom wiring pins.
    pins.insert(adc_config.adc.clk_bcm);
    pins.insert(adc_config.adc.mosi_bcm);
    pins.insert(adc_config.adc.miso_bcm);

    if (adc_config.adc.cs_bcm >= 0) {
        pins.insert(adc_config.adc.cs_bcm);
    }
    if (!adc_config.adc.bitbang) {
        // The Linux SPI controller owns the CE line for the selected spidev node
        // during transfers even when we also use a separate software CS wire.
        if (adc_config.adc.device.find("spidev0.1") != std::string::npos) pins.insert(7);  // CE1
        else if (adc_config.adc.device.find("spidev0.0") != std::string::npos) pins.insert(8); // CE0
    }
    return pins;
}

void removeReservedPinsFromRegistry(std::map<int, std::shared_ptr<PinState>>& registry, const std::set<int>& reserved_bcm) {
    for (auto it = registry.begin(); it != registry.end();) {
        if (reserved_bcm.count(it->second->bcm_pin)) {
            std::cout << "[GPIO] Hiding physical pin " << it->first << " / BCM" << it->second->bcm_pin
                      << " from GPIO control because it is reserved for ADC/SPI." << std::endl;
            it = registry.erase(it);
        } else {
            ++it;
        }
    }
}
}

int main(int argc, char* argv[]) {
    auto context = std::make_shared<SystemContext>();
    std::string listen_host = "0.0.0.0";
    int listen_port = 8080;
    bool custom_phys_list = false;
    bool custom_bcm_list = false;
    std::vector<int> custom_phys;
    std::vector<int> custom_bcm;
    bool adc_history_samples_explicit = false;
    uint64_t adc_history_ms = 30000; // default: keep 30 seconds of recent ADC history
    uint64_t adc_max_buffer_mb = 64; // cap combined CH0+CH1 ring buffer RAM

    AdcSampler::Config adc_config;
    adc_config.enabled = true;
    adc_config.sample_rate_hz = 8000;
    adc_config.history_samples = adc_config.sample_rate_hz * 30;
    adc_config.vref = 3.3;
    adc_config.adc.bitbang = false;
    adc_config.adc.device = "/dev/spidev0.0";
    adc_config.adc.speed_hz = 1000000;
    adc_config.adc.clk_bcm = 11;
    adc_config.adc.mosi_bcm = 10;
    adc_config.adc.miso_bcm = 9;
    adc_config.adc.cs_bcm = 8; // default MCP3202 CS is Raspberry Pi SPI0 CE0: physical pin 24 / BCM8

    // Load saved settings from config file before parsing CLI overrides
    ConfigManager temp_cfg_mgr(context->config_path);
    adc_config.adc_source = temp_cfg_mgr.getSetting("adc_source", "mcp3202-spidev");
    adc_config.rp2040_dev = temp_cfg_mgr.getSetting("rp2040_dev", "/dev/ttyACM0");

    try {
        for (int i = 1; i < argc; ++i) {
            std::string arg = argv[i];
            if (arg == "-h" || arg == "--help") {
                printUsage(argv[0], std::cout);
                return 0;
            } else if (arg == "--config") {
                context->config_path = requireValue(i, argc, argv, arg);
            } else if (arg == "--host") {
                listen_host = requireValue(i, argc, argv, arg);
            } else if (arg == "--port") {
                listen_port = parseIntStrict(requireValue(i, argc, argv, arg), arg);
                if (listen_port <= 0 || listen_port > 65535) throw std::runtime_error("--port must be 1..65535");
            } else if (arg == "--gpio-phys") {
                custom_phys_list = true;
                custom_phys = parseIntList(requireValue(i, argc, argv, arg), arg);
            } else if (arg == "--gpio-bcm") {
                custom_bcm_list = true;
                custom_bcm = parseIntList(requireValue(i, argc, argv, arg), arg);
            } else if (arg == "--adc-rp2040-dev") {
                adc_config.rp2040_dev = requireValue(i, argc, argv, arg);
            } else if (arg == "--adc-source") {
                std::string src = requireValue(i, argc, argv, arg);
                if (src != "mcp3202-spidev" && src != "rp2040") {
                    throw std::runtime_error("--adc-source must be 'mcp3202-spidev' or 'rp2040'");
                }
                adc_config.adc_source = src;
            } else if (arg == "--adc-channel") {
                std::cerr << "[ARGS] --adc-channel is ignored now; both MCP3202 channels are sampled and graphed." << std::endl;
                requireValue(i, argc, argv, arg);
            } else if (arg == "--adc-rate") {
                adc_config.sample_rate_hz = parseUInt32Strict(requireValue(i, argc, argv, arg), arg);
            } else if (arg == "--adc-history") {
                int v = parseIntStrict(requireValue(i, argc, argv, arg), arg);
                if (v <= 0) throw std::runtime_error("--adc-history must be positive");
                adc_config.history_samples = static_cast<size_t>(v);
                adc_history_samples_explicit = true;
            } else if (arg == "--adc-history-ms") {
                int v = parseIntStrict(requireValue(i, argc, argv, arg), arg);
                if (v <= 0) throw std::runtime_error("--adc-history-ms must be positive");
                adc_history_ms = static_cast<uint64_t>(v);
            } else if (arg == "--adc-max-buffer-mb") {
                int v = parseIntStrict(requireValue(i, argc, argv, arg), arg);
                if (v <= 0) throw std::runtime_error("--adc-max-buffer-mb must be positive");
                adc_max_buffer_mb = static_cast<uint64_t>(v);
            } else if (arg == "--adc-vref") {
                std::string s = requireValue(i, argc, argv, arg);
                size_t pos = 0;
                adc_config.vref = std::stod(s, &pos);
                if (pos != s.size() || adc_config.vref <= 0.0) throw std::runtime_error("--adc-vref must be a positive number");
            } else if (arg == "--adc-realtime") {
                adc_config.realtime = true;
            } else if (arg == "--adc-rt-priority") {
                int v = parseIntStrict(requireValue(i, argc, argv, arg), arg);
                if (v < 1 || v > 99) throw std::runtime_error("--adc-rt-priority must be 1..99");
                adc_config.realtime_priority = v;
            } else if (arg == "--adc-cpu") {
                int v = parseIntStrict(requireValue(i, argc, argv, arg), arg);
                if (v < -1) throw std::runtime_error("--adc-cpu must be -1 or a non-negative CPU index");
                adc_config.cpu_affinity = v;
            } else if (arg == "--spi-dev") {
                adc_config.adc.device = requireValue(i, argc, argv, arg);
            } else if (arg == "--spi-speed") {
                adc_config.adc.speed_hz = parseUInt32Strict(requireValue(i, argc, argv, arg), arg);
            } else if (arg == "--adc-cs-bcm") {
                adc_config.adc.cs_bcm = parseIntStrict(requireValue(i, argc, argv, arg), arg);
            } else if (arg == "--adc-clk-bcm") {
                adc_config.adc.clk_bcm = parseIntStrict(requireValue(i, argc, argv, arg), arg);
            } else if (arg == "--adc-mosi-bcm") {
                adc_config.adc.mosi_bcm = parseIntStrict(requireValue(i, argc, argv, arg), arg);
            } else if (arg == "--adc-miso-bcm") {
                adc_config.adc.miso_bcm = parseIntStrict(requireValue(i, argc, argv, arg), arg);
            } else if (arg == "--adc-gpio-chip") {
                adc_config.adc.gpio_chip = requireValue(i, argc, argv, arg);
            } else if (arg == "--adc-bitbang") {
                adc_config.adc.bitbang = true;
            } else if (arg == "--adc-hw-spi") {
                adc_config.adc.bitbang = false;
            } else if (arg == "--adc-disable" || arg == "--gpio-only" || arg == "--full-gpio") {
                adc_config.enabled = false;
            } else {
                throw std::runtime_error("Unknown option: " + arg);
            }
        }

        if (custom_phys_list && custom_bcm_list) {
            throw std::runtime_error("--gpio-phys and --gpio-bcm are mutually exclusive");
        }
    } catch (const std::exception& e) {
        std::cerr << "Argument error: " << e.what() << "\n\n";
        printUsage(argv[0], std::cerr);
        return 2;
    }

    if (adc_config.enabled && !adc_history_samples_explicit) {
        const uint64_t requested = (static_cast<uint64_t>(adc_config.sample_rate_hz) * adc_history_ms + 999ull) / 1000ull;
        const uint64_t max_bytes = adc_max_buffer_mb * 1024ull * 1024ull;
        const uint64_t max_frames = std::max<uint64_t>(1, max_bytes / (sizeof(uint16_t) * 2ull));
        adc_config.history_samples = static_cast<size_t>(std::max<uint64_t>(1, std::min<uint64_t>(requested, max_frames)));
        if (requested > max_frames) {
            std::cerr << "[ADC] Requested history " << adc_history_ms << " ms exceeds --adc-max-buffer-mb "
                      << adc_max_buffer_mb << "; capped to "
                      << ((adc_config.history_samples * 1000ull) / std::max<uint32_t>(1, adc_config.sample_rate_hz))
                      << " ms." << std::endl;
        }
    }

    std::map<int, std::shared_ptr<PinState>> registry;
    try {
        if (custom_phys_list) registry = buildPhysicalRegistry(custom_phys);
        else if (custom_bcm_list) registry = buildBcmRegistry(custom_bcm);
        else registry = buildFullHeaderRegistry();
    } catch (const std::exception& e) {
        std::cerr << "GPIO list error: " << e.what() << "\n\n";
        printUsage(argv[0], std::cerr);
        return 2;
    }

    std::unique_ptr<Ch1817Driver> ch1817_driver;
    try {
        ch1817_driver = std::make_unique<Ch1817Driver>(context);
        ch1817_driver->updateFromJson(ch1817_driver->settingsJson());
    } catch (const std::exception& e) {
        std::cerr << "Telephony configuration error: " << e.what() << "\n" << gpioHelpText() << std::endl;
        return 2;
    }

    std::set<int> reserved_gpio = configuredAdcPins(adc_config);
    try {
        if (ch1817_driver && ch1817_driver->settings().enabled) {
            reserved_gpio.insert(ch1817_driver->offhookBcm());
            reserved_gpio.insert(ch1817_driver->riBcm());
        }
    } catch (const std::exception& e) {
        std::cerr << "GPIO reservation error: " << e.what() << "\n" << gpioHelpText() << std::endl;
        return 2;
    }

    removeReservedPinsFromRegistry(registry, reserved_gpio);

    ConfigManager config_mgr(context->config_path);
    int loaded_timeout = 5000;
    if (config_mgr.load(registry, loaded_timeout)) {
        context->timeout_ms = loaded_timeout;
    }

    std::unique_ptr<AdcSampler> adc_sampler;
    std::unique_ptr<CallerIdDetector> caller_id_detector;
    if (adc_config.enabled) {
        adc_sampler = std::make_unique<AdcSampler>(adc_config);
        adc_sampler->start();
        caller_id_detector = std::make_unique<CallerIdDetector>(adc_sampler.get(), context);
        caller_id_detector->start();
    }

    if (ch1817_driver) ch1817_driver->start();

    GpioManager gpio_mgr(registry, reserved_gpio);
    gpio_mgr.start(context->timeout_ms);

    WebServer web_server(registry, config_mgr, gpio_mgr, context, adc_sampler.get(), caller_id_detector.get(), ch1817_driver.get(), reserved_gpio);
    
    std::cout << "=====================================================" << std::endl;
    std::cout << "Starting Modular CM4 GPIO" << (adc_config.enabled ? " + ADC (" + adc_config.adc_source + ")" : " Full-Control")
              << " Server on " << listen_host << ":" << listen_port << "..." << std::endl;
    std::cout << "Config path: " << context->config_path << std::endl;
    if (adc_config.enabled) {
        if (adc_config.adc_source == "rp2040") {
            std::cout << "ADC/graph: enabled\n"
                      << "  Source: RP2040 (USB CDC)\n"
                      << "  Device: " << adc_config.rp2040_dev << "\n"
                      << "  Frame rate: " << adc_config.sample_rate_hz << " Hz\n"
                      << "  History: " << adc_config.history_samples << " samples/channel" << std::endl;
        } else {
            std::cout << "ADC/graph: enabled"
                      << ", mode " << (adc_config.adc.bitbang ? "bitbang" : "hardware-spi")
                      << ", device " << adc_config.adc.device
                      << ", channels 0+1"
                      << ", frame rate " << adc_config.sample_rate_hz << " Hz"
                      << ", history " << adc_config.history_samples << " samples/channel"
                      << ", CS BCM " << adc_config.adc.cs_bcm
                      << ", CLK BCM " << adc_config.adc.clk_bcm
                      << ", MOSI BCM " << adc_config.adc.mosi_bcm
                      << ", MISO BCM " << adc_config.adc.miso_bcm
                      << ", realtime " << (adc_config.realtime ? "on" : "off")
                      << ", rt-priority " << adc_config.realtime_priority
                      << ", adc-cpu " << adc_config.cpu_affinity << std::endl;
        }
        std::cout << "Reserved ADC/SPI BCM pins hidden from GPIO control:";
        for (int bcm : reserved_gpio) std::cout << " " << bcm;
        std::cout << std::endl;
        if (custom_phys_list) {
            std::cout << "GPIO controls limited to requested physical pins, minus any ADC/SPI reservations." << std::endl;
        } else if (custom_bcm_list) {
            std::cout << "GPIO controls limited to requested BCM GPIOs, minus any ADC/SPI reservations." << std::endl;
        }
    } else if (custom_phys_list) {
        std::cout << "ADC/graph: disabled; custom GPIO-only mode exposes only requested physical pins." << std::endl;
    } else if (custom_bcm_list) {
        std::cout << "ADC/graph: disabled; custom GPIO-only mode exposes only requested BCM GPIOs." << std::endl;
    } else {
        std::cout << "ADC/graph: disabled; full GPIO control mode exposes all 40-pin-header GPIO lines." << std::endl;
    }
    std::cout << "GPIO-controlled pins exposed: " << registry.size() << std::endl;
    std::cout << "=====================================================" << std::endl;

    web_server.listen(listen_host, listen_port);

    if (caller_id_detector) caller_id_detector->stop();
    if (ch1817_driver) ch1817_driver->stop();
    if (adc_sampler) adc_sampler->stop();
    gpio_mgr.stop();
    return 0;
}
