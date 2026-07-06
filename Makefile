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

CORE_SRCS := crc32.cpp sha256.cpp base64.cpp framing.cpp adaptive.cpp link.cpp qr_encoder.cpp protocol.cpp fountain.cpp broadcast.cpp

CXXFLAGS_EXTRA :=
LDLIBS_EXTRA :=

ifeq ($(WITH_OPENCV),1)
CORE_SRCS += optical_link.cpp gui_common.cpp
CXXFLAGS_EXTRA += -DQRTP_WITH_OPENCV $(shell pkg-config --cflags opencv4)
LDLIBS_EXTRA += $(shell pkg-config --libs opencv4)
# opencv_contrib's wechat_qrcode module is confirmed present in Fedora's stock opencv4 pkg-config
# output, but genuinely absent from Debian 12's/Ubuntu 22.04-24.04's -- not universally packaged
# across distros (see optical_link.hpp's OpticalLinkConfig::use_wechat_qrcode comment). Detected
# via whether pkg-config actually links it in, same signal CMakeLists.txt checks via the header.
ifneq ($(shell pkg-config --libs opencv4 2>/dev/null | grep -o wechat_qrcode),)
CXXFLAGS_EXTRA += -DQRTP_HAVE_WECHAT_QRCODE
endif
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

# Native GTK3 GUI for Linux (opt-in, like WITH_OPENCV itself: requires gtk3-devel/libgtk-3-dev/
# gtk3 installed) — same role as the CMake-built qrtp_gui.exe on Windows, different toolkit.
# `make bin/qrtp_gui WITH_OPENCV=1` builds it explicitly; not part of the default `all` target,
# same as how the base CLI doesn't require GTK3 at all.
ifeq ($(WITH_OPENCV),1)
GTK_CFLAGS := $(shell pkg-config --cflags gtk+-3.0 2>/dev/null)
GTK_LIBS := $(shell pkg-config --libs gtk+-3.0 2>/dev/null)

$(BIN_DIR)/qrtp_gui: $(CORE_OBJS) $(BUILD_DIR)/gui_gtk.o | $(BIN_DIR)
	$(CXX) $(CXXFLAGS) -o $@ $^ $(LDFLAGS) $(LDLIBS) $(LDLIBS_EXTRA) $(GTK_LIBS)
	@mkdir -p $(BIN_DIR)/icon
	@cp -f linux/icon/qrtp.png $(BIN_DIR)/icon/qrtp.png 2>/dev/null || true

$(BUILD_DIR)/gui_gtk.o: $(SRC_DIR)/gui_gtk.cpp | $(BUILD_DIR)
	$(CXX) $(CXXFLAGS) $(GTK_CFLAGS) -c -o $@ $<
endif

test: $(BIN_DIR)/qrtp_tests
	./$(BIN_DIR)/qrtp_tests

clean:
	rm -rf $(BUILD_DIR) $(BIN_DIR)

-include $(DEPS)
