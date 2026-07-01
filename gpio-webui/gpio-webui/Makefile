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
DSP_TEST = dsp_selftest
AUDIO_PIPELINE_TEST = audio_pipeline_selftest
LINE_STATE_TEST = line_state_selftest
TELCO_TEST = telephony_coordinator_selftest
TELDIAG_TEST = telephony_diagnostics_selftest
AUDIO_FILTER_OBJS = libs/audio_filters/AudioFilters.o
FSK_OBJS = libs/fsk/Fsk.o
FILTER_PROFILE_OBJS = FilterProfileManager.o
DAC_PLAYBACK_OBJS = DacPlaybackFilters.o
PLAYBACK_OBJS = WavFile.o AudioPlaybackService.o
OBJS = main.o ConfigManager.o GpioManager.o WebServer.o MCP3202.o MCP4922.o DacOutput.o AdcSampler.o GwProtocol.o GwTransport.o GwDevice.o AudioProcessing.o $(AUDIO_FILTER_OBJS) $(FILTER_PROFILE_OBJS) $(DAC_PLAYBACK_OBJS) $(PLAYBACK_OBJS) CallerIdDetector.o $(FSK_OBJS) Ch1817Driver.o SignalProcessing.o LineStateDetector.o TelephonyCoordinator.o TelephonyDiagnostics.o
DSP_TEST_OBJS = dsp_selftest.o SignalProcessing.o
AUDIO_PIPELINE_TEST_OBJS = audio_pipeline_selftest.o AudioProcessing.o WavFile.o $(AUDIO_FILTER_OBJS)
LINE_STATE_TEST_OBJS = line_state_selftest.o LineStateDetector.o SignalProcessing.o AdcSampler.o GwProtocol.o GwTransport.o GwDevice.o MCP3202.o Ch1817Driver.o $(FILTER_PROFILE_OBJS) $(AUDIO_FILTER_OBJS)
TELCO_TEST_OBJS = telephony_coordinator_selftest.o TelephonyCoordinator.o LineStateDetector.o SignalProcessing.o AdcSampler.o GwProtocol.o GwTransport.o GwDevice.o MCP3202.o Ch1817Driver.o CallerIdDetector.o $(FSK_OBJS) $(FILTER_PROFILE_OBJS) $(AUDIO_FILTER_OBJS)
TELDIAG_TEST_OBJS = telephony_diagnostics_selftest.o TelephonyDiagnostics.o TelephonyCoordinator.o LineStateDetector.o SignalProcessing.o AdcSampler.o GwProtocol.o GwTransport.o GwDevice.o MCP3202.o Ch1817Driver.o CallerIdDetector.o $(FSK_OBJS) $(FILTER_PROFILE_OBJS) $(AUDIO_FILTER_OBJS)

all: rnnoise-status $(TARGET) $(DSP_TEST) $(AUDIO_PIPELINE_TEST) $(LINE_STATE_TEST) $(TELCO_TEST) $(TELDIAG_TEST)

rnnoise-status:
ifeq ($(HAVE_RNNOISE),1)
	@echo "RNNoise: enabled ($(RNNOISE_LIBS))"
else
	@echo "RNNoise: disabled (pkg-config rnnoise not found)"
endif

$(TARGET): $(OBJS)
	$(CXX) $(CXXFLAGS) -I. -o $(TARGET) $(OBJS) $(LDFLAGS)

$(DSP_TEST): $(DSP_TEST_OBJS)
	$(CXX) $(CXXFLAGS) -I. -o $(DSP_TEST) $(DSP_TEST_OBJS) -lpthread

$(AUDIO_PIPELINE_TEST): $(AUDIO_PIPELINE_TEST_OBJS)
	$(CXX) $(CXXFLAGS) -I. -o $(AUDIO_PIPELINE_TEST) $(AUDIO_PIPELINE_TEST_OBJS) $(LDFLAGS)

$(LINE_STATE_TEST): $(LINE_STATE_TEST_OBJS)
	$(CXX) $(CXXFLAGS) -I. -o $(LINE_STATE_TEST) $(LINE_STATE_TEST_OBJS) $(LDFLAGS)

$(TELCO_TEST): $(TELCO_TEST_OBJS)
	$(CXX) $(CXXFLAGS) -I. -o $(TELCO_TEST) $(TELCO_TEST_OBJS) $(LDFLAGS)

$(TELDIAG_TEST): $(TELDIAG_TEST_OBJS)
	$(CXX) $(CXXFLAGS) -I. -o $(TELDIAG_TEST) $(TELDIAG_TEST_OBJS) $(LDFLAGS)

main.o: main.cpp PinConfig.hpp ConfigManager.hpp GpioManager.hpp WebServer.hpp SystemContext.hpp SharedSignalBuffer.hpp AdcSampler.hpp MCP3202.hpp MCP4922.hpp DacOutput.hpp CallerIdDetector.hpp Ch1817Driver.hpp LineStateDetector.hpp TelephonyCoordinator.hpp TelephonyDiagnostics.hpp FilterProfileManager.hpp AudioPlaybackService.hpp HeaderPins.hpp httplib.h
	$(CXX) $(CXXFLAGS) -I. -c main.cpp -o main.o

ConfigManager.o: ConfigManager.cpp ConfigManager.hpp PinConfig.hpp
	$(CXX) $(CXXFLAGS) -I. -c ConfigManager.cpp -o ConfigManager.o

GpioManager.o: GpioManager.cpp GpioManager.hpp PinConfig.hpp
	$(CXX) $(CXXFLAGS) -I. -c GpioManager.cpp -o GpioManager.o

MCP3202.o: MCP3202.cpp MCP3202.hpp
	$(CXX) $(CXXFLAGS) -I. -c MCP3202.cpp -o MCP3202.o

MCP4922.o: MCP4922.cpp MCP4922.hpp
	$(CXX) $(CXXFLAGS) -I. -c MCP4922.cpp -o MCP4922.o

DacOutput.o: DacOutput.cpp DacOutput.hpp MCP4922.hpp AdcSampler.hpp GwProtocol.hpp GwTransport.hpp ../protocol/gw_protocol.h
	$(CXX) $(CXXFLAGS) -I. -c DacOutput.cpp -o DacOutput.o

AdcSampler.o: AdcSampler.cpp AdcSampler.hpp MCP3202.hpp SharedSignalBuffer.hpp GwProtocol.hpp ../protocol/gw_protocol.h
	$(CXX) $(CXXFLAGS) -I. -c AdcSampler.cpp -o AdcSampler.o

GwProtocol.o: GwProtocol.cpp GwProtocol.hpp ../protocol/gw_protocol.h
	$(CXX) $(CXXFLAGS) -I. -c GwProtocol.cpp -o GwProtocol.o

GwTransport.o: GwTransport.cpp GwTransport.hpp
	$(CXX) $(CXXFLAGS) -I. -c GwTransport.cpp -o GwTransport.o

GwDevice.o: GwDevice.cpp GwDevice.hpp GwProtocol.hpp ../protocol/gw_protocol.h
	$(CXX) $(CXXFLAGS) -I. -c GwDevice.cpp -o GwDevice.o

AudioProcessing.o: AudioProcessing.cpp AudioProcessing.hpp libs/audio_filters/AudioFilters.hpp
	$(CXX) $(CXXFLAGS) -I. -c AudioProcessing.cpp -o AudioProcessing.o

FilterProfileManager.o: FilterProfileManager.cpp FilterProfileManager.hpp SystemContext.hpp libs/audio_filters/AudioFilters.hpp
	$(CXX) $(CXXFLAGS) -I. -c FilterProfileManager.cpp -o FilterProfileManager.o

DacPlaybackFilters.o: DacPlaybackFilters.cpp DacPlaybackFilters.hpp AudioProcessing.hpp FilterProfileManager.hpp libs/audio_filters/AudioFilters.hpp
	$(CXX) $(CXXFLAGS) -I. -c DacPlaybackFilters.cpp -o DacPlaybackFilters.o

WavFile.o: WavFile.cpp WavFile.hpp AudioProcessing.hpp
	$(CXX) $(CXXFLAGS) -I. -c WavFile.cpp -o WavFile.o

AudioPlaybackService.o: AudioPlaybackService.cpp AudioPlaybackService.hpp DacOutput.hpp FilterProfileManager.hpp WavFile.hpp DacPlaybackFilters.hpp libs/audio_filters/AudioFilters.hpp
	$(CXX) $(CXXFLAGS) -I. -c AudioPlaybackService.cpp -o AudioPlaybackService.o

libs/audio_filters/AudioFilters.o: libs/audio_filters/AudioFilters.cpp libs/audio_filters/AudioFilters.hpp
	$(CXX) $(CXXFLAGS) -I. -c libs/audio_filters/AudioFilters.cpp -o libs/audio_filters/AudioFilters.o

libs/fsk/Fsk.o: libs/fsk/Fsk.cpp libs/fsk/Fsk.hpp
	$(CXX) $(CXXFLAGS) -I. -c libs/fsk/Fsk.cpp -o libs/fsk/Fsk.o

CallerIdDetector.o: CallerIdDetector.cpp CallerIdDetector.hpp AdcSampler.hpp SystemContext.hpp SharedSignalBuffer.hpp FilterProfileManager.hpp libs/fsk/Fsk.hpp libs/audio_filters/AudioFilters.hpp
	$(CXX) $(CXXFLAGS) -I. -c CallerIdDetector.cpp -o CallerIdDetector.o

Ch1817Driver.o: Ch1817Driver.cpp Ch1817Driver.hpp SystemContext.hpp HeaderPins.hpp SharedSignalBuffer.hpp
	$(CXX) $(CXXFLAGS) -I. -c Ch1817Driver.cpp -o Ch1817Driver.o

LineStateDetector.o: LineStateDetector.cpp LineStateDetector.hpp SignalProcessing.hpp AdcSampler.hpp Ch1817Driver.hpp SystemContext.hpp SharedSignalBuffer.hpp FilterProfileManager.hpp libs/audio_filters/AudioFilters.hpp
	$(CXX) $(CXXFLAGS) -I. -c LineStateDetector.cpp -o LineStateDetector.o

TelephonyCoordinator.o: TelephonyCoordinator.cpp TelephonyCoordinator.hpp CallerIdDetector.hpp Ch1817Driver.hpp LineStateDetector.hpp SystemContext.hpp
	$(CXX) $(CXXFLAGS) -I. -c TelephonyCoordinator.cpp -o TelephonyCoordinator.o

TelephonyDiagnostics.o: TelephonyDiagnostics.cpp TelephonyDiagnostics.hpp AdcSampler.hpp Ch1817Driver.hpp LineStateDetector.hpp TelephonyCoordinator.hpp SignalProcessing.hpp SystemContext.hpp FilterProfileManager.hpp libs/audio_filters/AudioFilters.hpp
	$(CXX) $(CXXFLAGS) -I. -c TelephonyDiagnostics.cpp -o TelephonyDiagnostics.o

SignalProcessing.o: SignalProcessing.cpp SignalProcessing.hpp SharedSignalBuffer.hpp
	$(CXX) $(CXXFLAGS) -I. -c SignalProcessing.cpp -o SignalProcessing.o

dsp_selftest.o: dsp_selftest.cpp SignalProcessing.hpp SharedSignalBuffer.hpp
	$(CXX) $(CXXFLAGS) -I. -c dsp_selftest.cpp -o dsp_selftest.o

audio_pipeline_selftest.o: audio_pipeline_selftest.cpp AudioProcessing.hpp WavFile.hpp libs/audio_filters/AudioFilters.hpp
	$(CXX) $(CXXFLAGS) -I. -c audio_pipeline_selftest.cpp -o audio_pipeline_selftest.o

line_state_selftest.o: line_state_selftest.cpp LineStateDetector.hpp SignalProcessing.hpp SharedSignalBuffer.hpp
	$(CXX) $(CXXFLAGS) -I. -c line_state_selftest.cpp -o line_state_selftest.o

telephony_coordinator_selftest.o: telephony_coordinator_selftest.cpp TelephonyCoordinator.hpp
	$(CXX) $(CXXFLAGS) -I. -c telephony_coordinator_selftest.cpp -o telephony_coordinator_selftest.o

telephony_diagnostics_selftest.o: telephony_diagnostics_selftest.cpp TelephonyDiagnostics.hpp
	$(CXX) $(CXXFLAGS) -I. -c telephony_diagnostics_selftest.cpp -o telephony_diagnostics_selftest.o

WebServer.o: WebServer.cpp WebServer.hpp PinConfig.hpp ConfigManager.hpp GpioManager.hpp SystemContext.hpp AdcSampler.hpp DacOutput.hpp MCP3202.hpp MCP4922.hpp AudioProcessing.hpp CallerIdDetector.hpp Ch1817Driver.hpp LineStateDetector.hpp TelephonyCoordinator.hpp TelephonyDiagnostics.hpp FilterProfileManager.hpp AudioPlaybackService.hpp SharedSignalBuffer.hpp httplib.h
	$(CXX) $(CXXFLAGS) -I. -c WebServer.cpp -o WebServer.o

clean:
	rm -f $(TARGET) $(DSP_TEST) $(AUDIO_PIPELINE_TEST) $(LINE_STATE_TEST) $(TELCO_TEST) $(TELDIAG_TEST) $(OBJS) $(AUDIO_FILTER_OBJS) $(FSK_OBJS) $(FILTER_PROFILE_OBJS) $(DAC_PLAYBACK_OBJS) $(PLAYBACK_OBJS) dsp_selftest.o audio_pipeline_selftest.o line_state_selftest.o telephony_coordinator_selftest.o telephony_diagnostics_selftest.o

.PHONY: all clean rnnoise-status
