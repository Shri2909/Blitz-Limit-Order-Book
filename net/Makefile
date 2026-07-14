# SPDX-License-Identifier: (GPL-2.0 OR BSD-2-Clause)
#
# net/Makefile
#
# HYDRA-LOB Phase 10 -- builds net/xdp_prog.bpf.c into net/xdp_prog.o.
#
# Deliberately separate from the C++ CMake build (CMakeLists.txt), per the
# project's build policy: BPF object code is produced by clang -target bpf,
# not the C++ toolchain, and gets loaded into the kernel at runtime via
# hydra::xdp::load_and_attach_xdp_program() (net/xdp_socket_user.cpp) or the
# xdp-loader CLI, not linked into the blitz_lob binary.
#
# Prerequisite (one-time):
#   sudo apt-get install -y libbpf-dev libxdp-dev clang
# (matches the libbpf/libxdp versions the C++ build links against -- see
# CMakeLists.txt's pkg-config discovery for the ENABLE_AFXDP option).
#
# Usage (from net/):
#   make            # -> xdp_prog.o
#   make clean

CLANG ?= clang
PKG_CONFIG ?= pkg-config

BPF_CFLAGS := $(shell $(PKG_CONFIG) --cflags libbpf libxdp 2>/dev/null) \
              -Wall -Wno-unused-value -Wno-pointer-sign \
              -Wno-compare-distinct-pointer-types -Wno-visibility \
              -fno-stack-protector

XDP_OBJ := xdp_prog.o

.PHONY: all clean check-deps

all: check-deps $(XDP_OBJ)

check-deps:
	@$(PKG_CONFIG) --exists libbpf libxdp || { \
		echo "ERROR: libbpf/libxdp development packages not found via pkg-config."; \
		echo "Install them with:"; \
		echo "  sudo apt-get install -y libbpf-dev libxdp-dev"; \
		exit 1; \
	}

$(XDP_OBJ): xdp_prog.bpf.c parsing_helpers.h Makefile
	$(CLANG) -O2 -target bpf $(BPF_CFLAGS) -c -g -o $@ $<

clean:
	rm -f $(XDP_OBJ)
