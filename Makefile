CXX ?= g++
CXXFLAGS ?= -O2 -std=c++17 -Wall -Wextra -Wpedantic

.PHONY: all test clean

all: test_sim

test_sim: tests/test_sim.cpp src/sim.hpp src/time_model.hpp
	$(CXX) $(CXXFLAGS) tests/test_sim.cpp -o $@

test: test_sim
	./test_sim

clean:
	rm -f test_sim
