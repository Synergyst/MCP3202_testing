#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace gw {

class Transport {
public:
    virtual ~Transport() = default;
    virtual bool open(std::string& error) = 0;
    virtual void close() = 0;
    virtual ssize_t read(uint8_t* data, size_t len) = 0;
    virtual ssize_t write(const uint8_t* data, size_t len) = 0;
    virtual bool isOpen() const = 0;
    virtual std::string description() const = 0;
};

class TtyTransport : public Transport {
public:
    explicit TtyTransport(std::string device);
    ~TtyTransport() override;

    bool open(std::string& error) override;
    void close() override;
    ssize_t read(uint8_t* data, size_t len) override;
    ssize_t write(const uint8_t* data, size_t len) override;
    bool isOpen() const override;
    std::string description() const override;
    int fd() const { return fd_; }

private:
    std::string device_;
    int fd_ = -1;
    bool configureRaw(std::string& error);
};

} // namespace gw
