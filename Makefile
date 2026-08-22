# ==============================================================================
# Makefile for ai_camera_replacement
# Drop-in replacement for Elegoo Centauri Carbon (and TinaLinux/ARM) ai_camera
# ==============================================================================

TARGET ?= ai_camera
BUILD_DIR ?= build

# Source files
SRCS = src/main.c \
       src/frame_ring.c \
       src/v4l2_camera.c \
       src/http_server.c \
       src/uds_ipc.c \
       src/timelapse_encoder.c

OBJS_ARM = $(patsubst src/%.c, $(BUILD_DIR)/arm/obj/%.o, $(SRCS))
DEPS_ARM = $(OBJS_ARM:.o=.d)

OBJS_HOST = $(patsubst src/%.c, $(BUILD_DIR)/host/obj/%.o, $(SRCS))
DEPS_HOST = $(OBJS_HOST:.o=.d)

VERSION ?= 0.2.0
GIT_VER = $(shell git describe --tags --always --dirty 2>/dev/null || echo "v$(VERSION)")

# Compiler flags
COMMON_CFLAGS = -O3 -Wall -Wextra -Isrc -D_GNU_SOURCE -DAPP_VERSION=\"$(VERSION)\" -DGIT_VERSION=\"$(GIT_VER)\" -MMD -MP

# ------------------------------------------------------------------------------
# ARM Cross-Compilation Configuration
# ------------------------------------------------------------------------------
TOOLCHAIN_DIR = $(BUILD_DIR)/toolchain
TOOLCHAIN_URL = https://musl.cc/armv7l-linux-musleabihf-cross.tgz

ifdef CROSS_COMPILE
  ARM_CC = $(CROSS_COMPILE)gcc
  ARM_STRIP = $(CROSS_COMPILE)strip
  CROSS_PREFIX_FLAG = --cross-prefix=$(CROSS_COMPILE)
else
  ARM_CC = $(TOOLCHAIN_DIR)/bin/armv7l-linux-musleabihf-gcc
  ARM_STRIP = $(TOOLCHAIN_DIR)/bin/armv7l-linux-musleabihf-strip
  CROSS_PREFIX_FLAG = --cross-prefix=$(abspath $(TOOLCHAIN_DIR))/bin/armv7l-linux-musleabihf-
endif

ARM_PREFIX = $(BUILD_DIR)/arm/sysroot
ARM_CFLAGS = $(COMMON_CFLAGS) -I$(ARM_PREFIX)/include
ARM_LDFLAGS = -L$(ARM_PREFIX)/lib -static -s -pthread -lx264 -lm

# ------------------------------------------------------------------------------
# Host / Native Compilation Configuration
# ------------------------------------------------------------------------------
HOST_CC ?= gcc
HOST_PREFIX = $(BUILD_DIR)/host/sysroot
HOST_CFLAGS = $(COMMON_CFLAGS) -I$(HOST_PREFIX)/include
HOST_LDFLAGS = -L$(HOST_PREFIX)/lib -pthread -lx264 -lm

# ------------------------------------------------------------------------------
# Targets
# ------------------------------------------------------------------------------
.PHONY: all arm native clean distclean help

all: arm

arm: $(TARGET)

native: $(BUILD_DIR)/ai_camera_native

# ------------------------------------------------------------------------------
# Auto-provisioning Toolchain & libx264
# ------------------------------------------------------------------------------
$(TOOLCHAIN_DIR)/bin/armv7l-linux-musleabihf-gcc:
	@echo "==> Downloading musl ARMv7l cross-toolchain..."
	@mkdir -p $(TOOLCHAIN_DIR)
	curl -L $(TOOLCHAIN_URL) | tar -C $(TOOLCHAIN_DIR) --strip-components=1 -xz

$(BUILD_DIR)/x264-src/.git:
	@echo "==> Cloning libx264..."
	@mkdir -p $(BUILD_DIR)
	git clone --depth 1 https://code.videolan.org/videolan/x264.git $(BUILD_DIR)/x264-src

$(ARM_PREFIX)/lib/libx264.a: $(BUILD_DIR)/x264-src/.git $(if $(CROSS_COMPILE),,$(TOOLCHAIN_DIR)/bin/armv7l-linux-musleabihf-gcc)
	@echo "==> Building libx264 for ARMv7l..."
	@mkdir -p $(BUILD_DIR)/x264-arm-build
	cd $(BUILD_DIR)/x264-src && \
	./configure \
		--host=arm-linux-gnueabihf \
		$(CROSS_PREFIX_FLAG) \
		--prefix=$(abspath $(ARM_PREFIX)) \
		--enable-static \
		--disable-cli \
		--disable-opencl \
		--disable-avs \
		--disable-ffms \
		--disable-gpac \
		--disable-lsmash \
		--disable-lavf \
		--disable-swscale \
		--disable-interlaced \
		--enable-pic \
		--extra-cflags="-O3" && \
	$(MAKE) -j$$(nproc) && \
	$(MAKE) install

$(HOST_PREFIX)/lib/libx264.a: $(BUILD_DIR)/x264-src/.git
	@echo "==> Building libx264 for Host..."
	@mkdir -p $(BUILD_DIR)/x264-host-build
	cd $(BUILD_DIR)/x264-src && \
	./configure \
		--prefix=$(abspath $(HOST_PREFIX)) \
		--enable-static \
		--disable-cli \
		--disable-opencl \
		--disable-avs \
		--disable-ffms \
		--disable-gpac \
		--disable-lsmash \
		--disable-lavf \
		--disable-swscale \
		--disable-interlaced \
		--enable-pic \
		--extra-cflags="-O3" && \
	$(MAKE) -j$$(nproc) && \
	$(MAKE) install

# ------------------------------------------------------------------------------
# Binary Compilation
# ------------------------------------------------------------------------------
$(BUILD_DIR)/arm/obj/%.o: src/%.c $(ARM_PREFIX)/lib/libx264.a
	@mkdir -p $(dir $@)
	$(ARM_CC) $(ARM_CFLAGS) -c $< -o $@

$(TARGET): $(OBJS_ARM)
	@echo "==> Linking $(TARGET) (ARMv7l static binary)..."
	$(ARM_CC) $(OBJS_ARM) $(ARM_LDFLAGS) -o $@
	@echo "==> Build complete: $(TARGET) ($$(du -h $(TARGET) | cut -f1))"

$(BUILD_DIR)/host/obj/%.o: src/%.c $(HOST_PREFIX)/lib/libx264.a
	@mkdir -p $(dir $@)
	$(HOST_CC) $(HOST_CFLAGS) -c $< -o $@

$(BUILD_DIR)/ai_camera_native: $(OBJS_HOST)
	@echo "==> Linking ai_camera_native (Host binary)..."
	$(HOST_CC) $(OBJS_HOST) $(HOST_LDFLAGS) -o $@
	@echo "==> Build complete: $@ ($$(du -h $@ | cut -f1))"

-include $(DEPS_ARM)
-include $(DEPS_HOST)

# ------------------------------------------------------------------------------
# Cleanup & Utilities
# ------------------------------------------------------------------------------
clean:
	@echo "==> Cleaning build objects and binaries..."
	rm -rf $(BUILD_DIR)/arm/obj $(BUILD_DIR)/host/obj $(TARGET) $(BUILD_DIR)/ai_camera_native ai_camera_native

distclean: clean
	@echo "==> Full clean of all downloaded toolchains and dependencies..."
	rm -rf $(BUILD_DIR)

help:
	@echo "Available targets:"
	@echo "  make              - Build static ARMv7l binary (ai_camera) [auto-fetches toolchain & x264]"
	@echo "  make native       - Build native host binary (build/ai_camera_native) for testing"
	@echo "  make clean        - Remove compiled object files and binaries"
	@echo "  make distclean    - Remove entire build directory (including cached toolchain & x264)"
	@echo ""
	@echo "Customization variables:"
	@echo "  CROSS_COMPILE     - Specify custom cross-compiler prefix (e.g. arm-linux-gnueabihf-)"
	@echo "  TARGET            - Custom output binary name (default: ai_camera)"
