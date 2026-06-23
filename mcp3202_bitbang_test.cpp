// Minimal MCP3202 bit-bang diagnostic, independent of gpio-webui.
// Build: g++ -std=c++17 -O2 -Wall -Wextra -o mcp3202_bitbang_test mcp3202_bitbang_test.cpp
// Example: ./mcp3202_bitbang_test --cs 16 --clk 11 --din 10 --dout 9 --samples 10 --delay-us 2
// Wiring names are MCP3202 names: DIN is Pi output -> MCP3202 pin 5, DOUT is Pi input <- MCP3202 pin 6.

#include <fcntl.h>
#include <sys/ioctl.h>
#include <unistd.h>
#include <linux/gpio.h>

#include <chrono>
#include <cerrno>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

struct Args {
    std::string chip = "/dev/gpiochip0";
    int cs = 16;
    int clk = 11;
    int din = 10;   // MOSI: Pi -> MCP3202 DIN pin 5
    int dout = 9;   // MISO: MCP3202 DOUT pin 6 -> Pi
    int samples = 20;
    int delay_us = 2;
    bool verbose_bits = false;
};

static std::string err(const std::string& s) { return s + ": " + std::strerror(errno); }

static int parse_int(const char* s, const std::string& what) {
    try {
        size_t p = 0;
        int v = std::stoi(s, &p, 10);
        if (p != std::string(s).size()) throw std::runtime_error("junk");
        return v;
    } catch (...) {
        throw std::runtime_error("Invalid integer for " + what + ": " + s);
    }
}

static void usage(const char* argv0) {
    std::cerr << "Usage: " << argv0 << " [--chip /dev/gpiochip0] --cs BCM --clk BCM --din BCM --dout BCM [--samples N] [--delay-us N] [--verbose-bits]\n"
              << "Defaults: --cs 16 --clk 11 --din 10 --dout 9 --samples 20 --delay-us 2\n"
              << "Expected for CH0=3.3V, CH1=GND with VDD/VREF=3.3V: CH0 near 4095, CH1 near 0.\n";
}

class Lines {
public:
    explicit Lines(const Args& a) : args(a) {
        chip_fd = ::open(args.chip.c_str(), O_RDONLY | O_CLOEXEC);
        if (chip_fd < 0) throw std::runtime_error(err("open " + args.chip));

        gpiohandle_request out_req{};
        out_req.lineoffsets[0] = args.cs;
        out_req.lineoffsets[1] = args.clk;
        out_req.lineoffsets[2] = args.din;
        out_req.default_values[0] = 1; // CS idle high
        out_req.default_values[1] = 0; // CLK idle low, SPI mode 0
        out_req.default_values[2] = 0; // DIN low
        out_req.flags = GPIOHANDLE_REQUEST_OUTPUT;
        out_req.lines = 3;
        std::snprintf(out_req.consumer_label, sizeof(out_req.consumer_label), "mcp3202_test_out");
        if (::ioctl(chip_fd, GPIO_GET_LINEHANDLE_IOCTL, &out_req) < 0) {
            throw std::runtime_error(err("request output lines cs/clk/din"));
        }
        out_fd = out_req.fd;

        gpiohandle_request in_req{};
        in_req.lineoffsets[0] = args.dout;
        in_req.flags = GPIOHANDLE_REQUEST_INPUT;
        in_req.lines = 1;
        std::snprintf(in_req.consumer_label, sizeof(in_req.consumer_label), "mcp3202_test_dout");
        if (::ioctl(chip_fd, GPIO_GET_LINEHANDLE_IOCTL, &in_req) < 0) {
            throw std::runtime_error(err("request input line dout"));
        }
        in_fd = in_req.fd;

        write(false, false, false); // CS low? no; this call sets cs=false if raw. Use setIdle next.
        setIdle();
    }

    ~Lines() {
        try { setIdle(); } catch (...) {}
        if (in_fd >= 0) ::close(in_fd);
        if (out_fd >= 0) ::close(out_fd);
        if (chip_fd >= 0) ::close(chip_fd);
    }

    void setIdle() { write(true, false, false); }

    void write(bool cs_high, bool clk_high, bool din_high) {
        gpiohandle_data d{};
        d.values[0] = cs_high ? 1 : 0;
        d.values[1] = clk_high ? 1 : 0;
        d.values[2] = din_high ? 1 : 0;
        if (::ioctl(out_fd, GPIOHANDLE_SET_LINE_VALUES_IOCTL, &d) < 0) {
            throw std::runtime_error(err("set output lines"));
        }
    }

    bool readDout() {
        gpiohandle_data d{};
        if (::ioctl(in_fd, GPIOHANDLE_GET_LINE_VALUES_IOCTL, &d) < 0) {
            throw std::runtime_error(err("read dout line"));
        }
        return d.values[0] != 0;
    }

    void delay() const {
        if (args.delay_us > 0) std::this_thread::sleep_for(std::chrono::microseconds(args.delay_us));
    }

private:
    Args args;
    int chip_fd = -1;
    int out_fd = -1;
    int in_fd = -1;
};

static bool clock_bit(Lines& io, bool din, std::string* bits = nullptr) {
    // MCP3202 supports SPI mode 0,0. Put DIN stable before rising edge; DOUT changes
    // on falling edge and is sampled after that falling edge.
    io.write(false, false, din);
    io.delay();
    io.write(false, true, din);
    io.delay();
    io.write(false, false, din);
    io.delay();
    bool dout = io.readDout();
    if (bits) bits->push_back(dout ? '1' : '0');
    return dout;
}

static uint16_t read_channel(Lines& io, int ch, bool verbose) {
    if (ch != 0 && ch != 1) throw std::runtime_error("channel must be 0 or 1");

    // Use the exact 24-clock, 8-bit-segment framing from MCP3202 datasheet Figure 6-1.
    // TX: 00000001, then SGL=1 ODD=ch MSBF=1 followed by don't-cares, then don't-cares.
    const uint8_t tx[3] = {0x01, static_cast<uint8_t>(0xA0 | (ch ? 0x40 : 0x00)), 0x00};
    uint8_t rx[3] = {0, 0, 0};
    std::string rx_bits;

    io.setIdle();
    io.delay();
    io.write(false, false, false); // CS low starts transaction
    io.delay();

    for (int byte = 0; byte < 3; ++byte) {
        for (int bit = 7; bit >= 0; --bit) {
            bool din = ((tx[byte] >> bit) & 1) != 0;
            bool dout = clock_bit(io, din, verbose ? &rx_bits : nullptr);
            rx[byte] = static_cast<uint8_t>((rx[byte] << 1) | (dout ? 1 : 0));
        }
    }

    io.setIdle();
    io.delay();

    uint16_t value = static_cast<uint16_t>(((rx[1] & 0x0F) << 8) | rx[2]);
    if (verbose) {
        std::cout << "CH" << ch
                  << " tx=[0x" << std::hex << static_cast<int>(tx[0])
                  << " 0x" << static_cast<int>(tx[1])
                  << " 0x" << static_cast<int>(tx[2]) << std::dec << "]"
                  << " rx_bits=" << rx_bits
                  << " rx=[0x" << std::hex << static_cast<int>(rx[0])
                  << " 0x" << static_cast<int>(rx[1])
                  << " 0x" << static_cast<int>(rx[2]) << std::dec << "]"
                  << " value=" << value << "\n";
    }
    return value;
}

int main(int argc, char** argv) {
    Args a;
    try {
        for (int i = 1; i < argc; ++i) {
            std::string s = argv[i];
            auto need = [&](const std::string& opt) -> const char* {
                if (i + 1 >= argc) throw std::runtime_error("Missing value for " + opt);
                return argv[++i];
            };
            if (s == "--help" || s == "-h") { usage(argv[0]); return 0; }
            else if (s == "--chip") a.chip = need(s);
            else if (s == "--cs") a.cs = parse_int(need(s), s);
            else if (s == "--clk") a.clk = parse_int(need(s), s);
            else if (s == "--din" || s == "--mosi") a.din = parse_int(need(s), s);
            else if (s == "--dout" || s == "--miso") a.dout = parse_int(need(s), s);
            else if (s == "--samples") a.samples = parse_int(need(s), s);
            else if (s == "--delay-us") a.delay_us = parse_int(need(s), s);
            else if (s == "--verbose-bits") a.verbose_bits = true;
            else throw std::runtime_error("Unknown option " + s);
        }
        if (a.samples <= 0) throw std::runtime_error("--samples must be positive");
        if (a.delay_us < 0) throw std::runtime_error("--delay-us must be >= 0");

        std::cout << "MCP3202 bit-bang test using " << a.chip
                  << " CS=BCM" << a.cs
                  << " CLK=BCM" << a.clk
                  << " DIN/MOSI=BCM" << a.din
                  << " DOUT/MISO=BCM" << a.dout
                  << " delay_us=" << a.delay_us << "\n";
        std::cout << "Expected with CH0=3.3V and CH1=GND: CH0 ~= 4095, CH1 ~= 0.\n";

        Lines io(a);
        for (int i = 0; i < a.samples; ++i) {
            uint16_t ch0 = read_channel(io, 0, a.verbose_bits);
            uint16_t ch1 = read_channel(io, 1, a.verbose_bits);
            std::cout << "sample " << i << ": CH0=" << ch0 << " CH1=" << ch1;
            if (ch0 > 3500 && ch1 < 500) std::cout << "  OK";
            else std::cout << "  UNEXPECTED";
            std::cout << "\n";
            std::this_thread::sleep_for(std::chrono::milliseconds(250));
        }
        io.setIdle();
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "ERROR: " << e.what() << "\n";
        usage(argv[0]);
        return 2;
    }
}
