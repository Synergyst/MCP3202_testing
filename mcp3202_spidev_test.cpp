#include <fcntl.h>
#include <linux/spi/spidev.h>
#include <sys/ioctl.h>
#include <unistd.h>
#include <cerrno>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <string>
#include <chrono>
#include <thread>

static std::string emsg(const std::string& s){ return s + ": " + std::strerror(errno); }
static int xfer(int fd, uint32_t speed, int ch){
  uint8_t tx[3] = {0x01, static_cast<uint8_t>(0xA0 | (ch ? 0x40 : 0x00)), 0x00};
  uint8_t rx[3] = {0,0,0};
  spi_ioc_transfer tr{};
  tr.tx_buf = (unsigned long)tx; tr.rx_buf = (unsigned long)rx; tr.len=3; tr.speed_hz=speed; tr.bits_per_word=8;
  if(ioctl(fd, SPI_IOC_MESSAGE(1), &tr) < 1) throw std::runtime_error(emsg("SPI_IOC_MESSAGE"));
  int val = ((rx[1] & 0x0f) << 8) | rx[2];
  std::cout << "CH" << ch << " tx=[0x" << std::hex << (int)tx[0] << " 0x" << (int)tx[1] << " 0x" << (int)tx[2]
            << "] rx=[0x" << (int)rx[0] << " 0x" << (int)rx[1] << " 0x" << (int)rx[2] << std::dec << "] val=" << val << "\n";
  return val;
}
int main(int argc,char**argv){
  std::string dev="/dev/spidev0.0"; uint32_t speed=900000; uint8_t mode=0,bits=8; int samples=10;
  for(int i=1;i<argc;i++){std::string a=argv[i]; if(a=="--dev") dev=argv[++i]; else if(a=="--speed") speed=std::stoul(argv[++i]); else if(a=="--mode") mode=std::stoul(argv[++i]); else if(a=="--samples") samples=std::stoi(argv[++i]);}
  int fd=open(dev.c_str(),O_RDWR|O_CLOEXEC); if(fd<0) throw std::runtime_error(emsg("open "+dev));
  if(ioctl(fd,SPI_IOC_WR_MODE,&mode)<0) throw std::runtime_error(emsg("WR_MODE"));
  if(ioctl(fd,SPI_IOC_WR_BITS_PER_WORD,&bits)<0) throw std::runtime_error(emsg("WR_BITS"));
  if(ioctl(fd,SPI_IOC_WR_MAX_SPEED_HZ,&speed)<0) throw std::runtime_error(emsg("WR_SPEED"));
  std::cout << "dev="<<dev<<" mode="<<(int)mode<<" speed="<<speed<<" expected both high => both near 4095\n";
  for(int i=0;i<samples;i++){ xfer(fd,speed,0); xfer(fd,speed,1); std::this_thread::sleep_for(std::chrono::milliseconds(200)); }
  close(fd);
}
