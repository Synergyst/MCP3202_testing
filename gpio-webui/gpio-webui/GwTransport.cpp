#include "GwTransport.hpp"

#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <termios.h>
#include <unistd.h>

namespace gw {

TtyTransport::TtyTransport(std::string device) : device_(std::move(device)) {}
TtyTransport::~TtyTransport() { close(); }

bool TtyTransport::open(std::string& error) {
    close();
    fd_ = ::open(device_.c_str(), O_RDWR | O_NOCTTY | O_CLOEXEC | O_NONBLOCK);
    if (fd_ < 0) {
        error = "open " + device_ + ": " + std::strerror(errno);
        return false;
    }
    if (!configureRaw(error)) {
        close();
        return false;
    }
    return true;
}

void TtyTransport::close() {
    if (fd_ >= 0) {
        ::close(fd_);
        fd_ = -1;
    }
}

ssize_t TtyTransport::read(uint8_t* data, size_t len) {
    if (fd_ < 0) return -1;
    return ::read(fd_, data, len);
}

ssize_t TtyTransport::write(const uint8_t* data, size_t len) {
    if (fd_ < 0) return -1;
    return ::write(fd_, data, len);
}

bool TtyTransport::isOpen() const { return fd_ >= 0; }
std::string TtyTransport::description() const { return "tty:" + device_; }

bool TtyTransport::configureRaw(std::string& error) {
    termios tio{};
    if (::tcgetattr(fd_, &tio) != 0) {
        error = "tcgetattr " + device_ + ": " + std::strerror(errno);
        return false;
    }
    ::cfmakeraw(&tio);
    ::cfsetispeed(&tio, B115200);
    ::cfsetospeed(&tio, B115200);
    tio.c_cflag |= (CLOCAL | CREAD);
    tio.c_cflag &= ~CRTSCTS;
    tio.c_cc[VMIN] = 0;
    tio.c_cc[VTIME] = 1;
    if (::tcsetattr(fd_, TCSANOW, &tio) != 0) {
        error = "tcsetattr " + device_ + ": " + std::strerror(errno);
        return false;
    }
    return true;
}

} // namespace gw
