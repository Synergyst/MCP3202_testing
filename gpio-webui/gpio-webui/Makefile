CXX = g++
CXXFLAGS = -std=c++17 -O2 -w -Wno-psabi
LDFLAGS = -lgpiodcxx -lgpiod -lpthread
TARGET = cm4_gpio_server
OBJS = main.o ConfigManager.o GpioManager.o WebServer.o MCP3202.o AdcSampler.o

all: $(TARGET)

$(TARGET): $(OBJS)
	$(CXX) $(CXXFLAGS) -I. -o $(TARGET) $(OBJS) $(LDFLAGS)

main.o: main.cpp PinConfig.hpp ConfigManager.hpp GpioManager.hpp WebServer.hpp SystemContext.hpp AdcSampler.hpp MCP3202.hpp httplib.h
	$(CXX) $(CXXFLAGS) -I. -c main.cpp -o main.o

ConfigManager.o: ConfigManager.cpp ConfigManager.hpp PinConfig.hpp
	$(CXX) $(CXXFLAGS) -I. -c ConfigManager.cpp -o ConfigManager.o

GpioManager.o: GpioManager.cpp GpioManager.hpp PinConfig.hpp
	$(CXX) $(CXXFLAGS) -I. -c GpioManager.cpp -o GpioManager.o

MCP3202.o: MCP3202.cpp MCP3202.hpp
	$(CXX) $(CXXFLAGS) -I. -c MCP3202.cpp -o MCP3202.o

AdcSampler.o: AdcSampler.cpp AdcSampler.hpp MCP3202.hpp
	$(CXX) $(CXXFLAGS) -I. -c AdcSampler.cpp -o AdcSampler.o

WebServer.o: WebServer.cpp WebServer.hpp PinConfig.hpp ConfigManager.hpp GpioManager.hpp SystemContext.hpp AdcSampler.hpp MCP3202.hpp httplib.h
	$(CXX) $(CXXFLAGS) -I. -c WebServer.cpp -o WebServer.o

clean:
	rm -f $(TARGET) $(OBJS)

.PHONY: all clean
