CXX ?= g++
CXXFLAGS ?= -std=c++20 -O3 -march=native -Wall -Wextra -Wpedantic -pthread
INCLUDES = -I./include

BUILD_DIR = build
BIN_DIR = bin

TARGETS = $(BIN_DIR)/message_queue_demo \
          $(BIN_DIR)/test_spsc \
          $(BIN_DIR)/test_mpmc \
          $(BIN_DIR)/false_sharing_bench \
          $(BIN_DIR)/spsc_throughput \
          $(BIN_DIR)/spsc_latency_rtt \
          $(BIN_DIR)/mpmc_bench

.PHONY: all clean test bench run

all: $(TARGETS)

$(BIN_DIR):
	mkdir -p $(BIN_DIR)

$(BIN_DIR)/message_queue_demo: src/main.cpp include/lockfree/*.hpp | $(BIN_DIR)
	$(CXX) $(CXXFLAGS) $(INCLUDES) $< -o $@

$(BIN_DIR)/test_spsc: tests/test_spsc.cpp include/lockfree/*.hpp | $(BIN_DIR)
	$(CXX) $(CXXFLAGS) $(INCLUDES) $< -o $@

$(BIN_DIR)/test_mpmc: tests/test_mpmc.cpp include/lockfree/*.hpp | $(BIN_DIR)
	$(CXX) $(CXXFLAGS) $(INCLUDES) $< -o $@

$(BIN_DIR)/false_sharing_bench: benchmarks/false_sharing_bench.cpp include/lockfree/*.hpp | $(BIN_DIR)
	$(CXX) $(CXXFLAGS) $(INCLUDES) $< -o $@

$(BIN_DIR)/spsc_throughput: benchmarks/spsc_throughput.cpp include/lockfree/*.hpp | $(BIN_DIR)
	$(CXX) $(CXXFLAGS) $(INCLUDES) $< -o $@

$(BIN_DIR)/spsc_latency_rtt: benchmarks/spsc_latency_rtt.cpp include/lockfree/*.hpp | $(BIN_DIR)
	$(CXX) $(CXXFLAGS) $(INCLUDES) $< -o $@

$(BIN_DIR)/mpmc_bench: benchmarks/mpmc_bench.cpp include/lockfree/*.hpp | $(BIN_DIR)
	$(CXX) $(CXXFLAGS) $(INCLUDES) $< -o $@

test: $(BIN_DIR)/test_spsc $(BIN_DIR)/test_mpmc
	@echo "=========================================="
	@echo " Running Test Suite"
	@echo "=========================================="
	@./$(BIN_DIR)/test_spsc
	@echo ""
	@./$(BIN_DIR)/test_mpmc

bench_false_sharing: $(BIN_DIR)/false_sharing_bench
	@./$(BIN_DIR)/false_sharing_bench

bench_throughput: $(BIN_DIR)/spsc_throughput
	@./$(BIN_DIR)/spsc_throughput

bench_latency: $(BIN_DIR)/spsc_latency_rtt
	@./$(BIN_DIR)/spsc_latency_rtt

bench_mpmc: $(BIN_DIR)/mpmc_bench
	@./$(BIN_DIR)/mpmc_bench

bench: bench_false_sharing bench_throughput bench_latency bench_mpmc

run: $(BIN_DIR)/message_queue_demo
	@./$(BIN_DIR)/message_queue_demo

clean:
	rm -rf $(BIN_DIR)
