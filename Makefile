CXX      ?= g++
CXXFLAGS ?= -std=c++17 -O2 -g -Wall -Wextra -Wpedantic -Werror -Iinclude
SAN      ?=
LIB_SRC  := src/sim_transport.cpp src/peripheral.cpp src/host.cpp
HDRS     := $(wildcard include/rpp/*.hpp)

all: build/rpp_tests build/rpp_demo

build/rpp_tests: $(LIB_SRC) tests/test_main.cpp $(HDRS)
	@mkdir -p build
	$(CXX) $(CXXFLAGS) $(SAN) $(LIB_SRC) tests/test_main.cpp -o $@

build/rpp_demo: $(LIB_SRC) src/demo.cpp $(HDRS)
	@mkdir -p build
	$(CXX) $(CXXFLAGS) $(SAN) $(LIB_SRC) src/demo.cpp -o $@

test: build/rpp_tests
	./build/rpp_tests

# Same tests under AddressSanitizer + UBSan
sanitize:
	$(MAKE) clean
	$(MAKE) SAN="-fsanitize=address,undefined -fno-sanitize-recover=all" test

demo: build/rpp_demo
	./build/rpp_demo

clean:
	rm -rf build

.PHONY: all test sanitize demo clean
