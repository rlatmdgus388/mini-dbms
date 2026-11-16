CXX      ?= c++
BUILD    ?= build
OPT      ?= -O2 -g
SAN      ?=
WARN     := -Wall -Wextra -Wpedantic
CXXFLAGS := -std=c++17 $(OPT) $(WARN) -Isrc $(SAN) -MMD -MP
LDLIBS   := -pthread

LIB_SRCS  := $(shell find src -name '*.cpp' ! -path 'src/cli/*' 2>/dev/null)
LIB_OBJS  := $(LIB_SRCS:%.cpp=$(BUILD)/obj/%.o)
TEST_SRCS := $(wildcard tests/*.cpp)
TEST_OBJS := $(TEST_SRCS:%.cpp=$(BUILD)/obj/%.o)

BINS := $(BUILD)/minidb
ifneq ($(wildcard src/cli/server_main.cpp),)
BINS += $(BUILD)/minidb_server
endif
ifneq ($(wildcard bench/bench.cpp),)
BENCH_BIN := $(BUILD)/bench
endif

.PHONY: all test tsan bench clean
all: $(BINS) $(BENCH_BIN) $(BUILD)/tests

$(BUILD)/minidb: $(BUILD)/obj/src/cli/main.o $(LIB_OBJS)
	$(CXX) $(CXXFLAGS) $^ -o $@ $(LDLIBS)

$(BUILD)/minidb_server: $(BUILD)/obj/src/cli/server_main.o $(LIB_OBJS)
	$(CXX) $(CXXFLAGS) $^ -o $@ $(LDLIBS)

$(BUILD)/bench: $(BUILD)/obj/bench/bench.o $(LIB_OBJS)
	$(CXX) $(CXXFLAGS) $^ -o $@ $(LDLIBS)

$(BUILD)/tests: $(TEST_OBJS) $(LIB_OBJS)
	$(CXX) $(CXXFLAGS) $^ -o $@ $(LDLIBS)

$(BUILD)/obj/%.o: %.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -c $< -o $@

test: all
	$(BUILD)/tests
	@if [ -f tests/functional_cli.sh ]; then sh tests/functional_cli.sh $(BUILD); fi
	@if [ -f tests/functional_server.sh ]; then sh tests/functional_server.sh $(BUILD); fi

# ThreadSanitizer 빌드로 전체 테스트(동시성 테스트 포함)를 실행한다.
tsan:
	$(MAKE) BUILD=build/tsan OPT="-O1 -g" SAN="-fsanitize=thread" build/tsan/tests
	build/tsan/tests

bench: $(BUILD)/bench
	$(BUILD)/bench

clean:
	rm -rf build

-include $(shell find $(BUILD)/obj -name "*.d" 2>/dev/null)
