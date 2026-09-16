CXX ?= g++
CXXFLAGS ?= -O2 -std=c++17 -Wall -Wextra -Wpedantic

.PHONY: all test clean

all: test_sim trace_bench

test_sim: tests/test_sim.cpp src/sim.hpp src/time_model.hpp src/prefetchers.hpp
	$(CXX) $(CXXFLAGS) tests/test_sim.cpp -o $@

trace_bench: src/trace_bench.cpp src/sim.hpp src/oracle_trace.hpp src/prefetchers.hpp
	$(CXX) $(CXXFLAGS) src/trace_bench.cpp -o $@

test: test_sim trace_bench
	./test_sim

clean:
	rm -f test_sim trace_bench
