CXX ?= c++
CXXFLAGS ?= -O2 -std=c++17 -Wall -Wextra -Wpedantic
ROLE ?= operator
BINDIR := $(abspath ../lineFollowerDistributedBin)
HEADERS = $(wildcard *.hpp vendor/*.h)

.PHONY: all clean

ifeq ($(ROLE),controller)
all: $(BINDIR)/controller $(BINDIR)/gpio_clock $(BINDIR)/clock_probe $(BINDIR)/config.json
$(BINDIR)/config.json: config.controller.json | $(BINDIR)
	cp config.controller.json $@
$(BINDIR)/controller: controller.cpp $(HEADERS) | $(BINDIR)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) controller.cpp -o $@ $(LDFLAGS)
$(BINDIR)/gpio_clock: gpio_clock.cpp | $(BINDIR)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -pthread gpio_clock.cpp -o $@ $(LDFLAGS)
$(BINDIR)/clock_probe: clock_probe.cpp | $(BINDIR)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -pthread clock_probe.cpp -o $@ $(LDFLAGS)
else ifeq ($(ROLE),operator)
all: $(BINDIR)/run_operator $(BINDIR)/clock_probe $(BINDIR)/config.json
$(BINDIR)/config.json: config.operator.json | $(BINDIR)
	cp config.operator.json $@
$(BINDIR)/run_operator: operator.cpp $(HEADERS) | $(BINDIR)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) operator.cpp -o $@ $(LDFLAGS)
$(BINDIR)/clock_probe: clock_probe.cpp | $(BINDIR)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -pthread clock_probe.cpp -o $@ $(LDFLAGS)
else
$(error ROLE must be controller or operator)
endif

$(BINDIR):
	mkdir -p $@

clean:
	rm -f $(BINDIR)/controller $(BINDIR)/gpio_clock $(BINDIR)/clock_probe $(BINDIR)/run_operator $(BINDIR)/config.json
