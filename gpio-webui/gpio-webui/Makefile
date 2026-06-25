CXX = g++
RNNOISE_CFLAGS := $(shell pkg-config --cflags rnnoise 2>/dev/null)
RNNOISE_LIBS := $(shell pkg-config --libs rnnoise 2>/dev/null)
HAVE_RNNOISE := $(shell pkg-config --exists rnnoise 2>/dev/null && echo 1)

CXXFLAGS = -std=c++17 -O2 -w -Wno-psabi
ifeq ($(HAVE_RNNOISE),1)
CXXFLAGS += -DHAVE_RNNOISE $(RNNOISE_CFLAGS)
endif
LDFLAGS = -lgpiodcxx -lgpiod -lpthread
ifeq ($(HAVE_RNNOISE),1)
LDFLAGS += $(RNNOISE_LIBS)
endif
TARGET = cm4_gpio_server
OBJS = main.o ConfigManager.o GpioManager.o WebServer.o MCP3202.o AdcSampler.o AudioProcessing.o CallerIdDetector.o Ch1817Driver.o

all: rnnoise-status $(TARGET)

rnnoise-status:
ifeq ($(HAVE_RNNOISE),1)
	@echo "RNNoise: enabled ($(RNNOISE_LIBS))"
else
	@echo "RNNoise: disabled (pkg-config rnnoise not found)"
endif

$(TARGET): $(OBJS)
	$(CXX) $(CXXFLAGS) -I. -o $(TARGET) $(OBJS) $(LDFLAGS)

main.o: main.cpp PinConfig.hpp ConfigManager.hpp GpioManager.hpp WebServer.hpp SystemContext.hpp AdcSampler.hpp MCP3202.hpp CallerIdDetector.hpp Ch1817Driver.hpp HeaderPins.hpp httplib.h
	$(CXX) $(CXXFLAGS) -I. -c main.cpp -o main.o

ConfigManager.o: ConfigManager.cpp ConfigManager.hpp PinConfig.hpp
	$(CXX) $(CXXFLAGS) -I. -c ConfigManager.cpp -o ConfigManager.o

GpioManager.o: GpioManager.cpp GpioManager.hpp PinConfig.hpp
	$(CXX) $(CXXFLAGS) -I. -c GpioManager.cpp -o GpioManager.o

MCP3202.o: MCP3202.cpp MCP3202.hpp
	$(CXX) $(CXXFLAGS) -I. -c MCP3202.cpp -o MCP3202.o

AdcSampler.o: AdcSampler.cpp AdcSampler.hpp MCP3202.hpp
	$(CXX) $(CXXFLAGS) -I. -c AdcSampler.cpp -o AdcSampler.o

AudioProcessing.o: AudioProcessing.cpp AudioProcessing.hpp
	$(CXX) $(CXXFLAGS) -I. -c AudioProcessing.cpp -o AudioProcessing.o

CallerIdDetector.o: CallerIdDetector.cpp CallerIdDetector.hpp AdcSampler.hpp SystemContext.hpp
	$(CXX) $(CXXFLAGS) -I. -c CallerIdDetector.cpp -o CallerIdDetector.o

Ch1817Driver.o: Ch1817Driver.cpp Ch1817Driver.hpp SystemContext.hpp HeaderPins.hpp
	$(CXX) $(CXXFLAGS) -I. -c Ch1817Driver.cpp -o Ch1817Driver.o


WebServer.o: WebServer.cpp WebServer.hpp PinConfig.hpp ConfigManager.hpp GpioManager.hpp SystemContext.hpp AdcSampler.hpp MCP3202.hpp AudioProcessing.hpp CallerIdDetector.hpp Ch1817Driver.hpp httplib.h
	$(CXX) $(CXXFLAGS) -I. -c WebServer.cpp -o WebServer.o

clean:
	rm -f $(TARGET) $(OBJS)

.PHONY: all clean rnnoise-status
