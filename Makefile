CXX ?= g++
CXXSTD := -std=c++17
WARN := -Wall -Wextra
OPT := -O2 -g
INCLUDES := -Iinclude
LDFLAGS := -pthread
LDLIBS := -ldl

SRC_DIR := src
TEST_DIR := tests
BUILD_DIR := build
BIN_DIR := bin

CORE_SRCS := crc32.cpp sha256.cpp framing.cpp adaptive.cpp link.cpp qr_encoder.cpp protocol.cpp

CXXFLAGS_EXTRA :=
LDLIBS_EXTRA :=

ifeq ($(WITH_OPENCV),1)
CORE_SRCS += optical_link.cpp
CXXFLAGS_EXTRA += -DQRTP_WITH_OPENCV $(shell pkg-config --cflags opencv4)
LDLIBS_EXTRA += $(shell pkg-config --libs opencv4)
endif

DEPFLAGS := -MMD -MP
CXXFLAGS := $(CXXSTD) $(WARN) $(OPT) $(INCLUDES) $(DEPFLAGS) $(CXXFLAGS_EXTRA)

CORE_OBJS := $(addprefix $(BUILD_DIR)/,$(CORE_SRCS:.cpp=.o))
DEPS := $(CORE_OBJS:.o=.d) $(BUILD_DIR)/main.d $(BUILD_DIR)/test_main.d

.PHONY: all test clean

all: $(BIN_DIR)/qrtp

$(BIN_DIR)/qrtp: $(CORE_OBJS) $(BUILD_DIR)/main.o | $(BIN_DIR)
	$(CXX) $(CXXFLAGS) -o $@ $^ $(LDFLAGS) $(LDLIBS) $(LDLIBS_EXTRA)

$(BIN_DIR)/qrtp_tests: $(CORE_OBJS) $(BUILD_DIR)/test_main.o | $(BIN_DIR)
	$(CXX) $(CXXFLAGS) -o $@ $^ $(LDFLAGS) $(LDLIBS) $(LDLIBS_EXTRA)

$(BUILD_DIR)/%.o: $(SRC_DIR)/%.cpp | $(BUILD_DIR)
	$(CXX) $(CXXFLAGS) -c -o $@ $<

$(BUILD_DIR)/test_main.o: $(TEST_DIR)/test_main.cpp | $(BUILD_DIR)
	$(CXX) $(CXXFLAGS) -c -o $@ $<

$(BUILD_DIR) $(BIN_DIR):
	mkdir -p $@

test: $(BIN_DIR)/qrtp_tests
	./$(BIN_DIR)/qrtp_tests

clean:
	rm -rf $(BUILD_DIR) $(BIN_DIR)

-include $(DEPS)
