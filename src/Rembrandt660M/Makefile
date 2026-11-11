SHELL := /bin/bash
.SHELLFLAGS := -euo pipefail -c
.DELETE_ON_ERROR:
# Rembrandt660M: Lilu plugin for the Radeon 660M on macOS 26.7 (25G229). Inputs: see README.md, Building.
ROOT   := ../..
SDK    ?= $(ROOT)/deps/MacKernelSDK
LILU   ?= ../../build/Lilu-1.7.3-ABI-checked/Lilu
DARWIN_LD ?= /usr/bin/ld
# 25G229 Kernel Debug Kit (LC_UUIDs pinned in tools/verify_symbol_manifest.py)
KDK    ?= /Library/Developer/KDKs/KDK_26.7_25G229.kdk
KDK_EXTENSIONS ?= $(KDK)/System/Library/Extensions
KERNEL_INPUT ?= $(KDK)/System/Library/Kernels/kernel
LILU_RUNTIME ?= $(ROOT)/build/runtime-inputs/Lilu
NAME   := Rembrandt660M
VER    := 0.1.0
KFLAGS := -target x86_64-apple-macos10.15 -mkernel -nostdinc -fno-builtin -fno-common -mno-red-zone -O2 \
          -fno-asynchronous-unwind-tables -DKERNEL -DKERNEL_PRIVATE -DDRIVER_PRIVATE -DAPPLE -DNeXT \
          -I$(SDK)/Headers -I$(LILU)
CXXFLAGS := $(KFLAGS) -std=c++17 -fapple-kext -nostdlib -nostdinc++ -fno-rtti -fno-exceptions \
            -fno-use-cxa-atexit \
            -DPRODUCT_NAME=$(NAME) -DMODULE_VERSION=$(VER) -Wno-deprecated-declarations
OBJS   := kern_start.o routing.o mmio.o smu.o smu_powerplay.o gfxclk.o psp_compat.o gpu_contract.o golden.o dmub.o display.o firmware.o probe.o vmwatch.o sleep.o usb4.o umdpatch.o disklog.o iogdiag.o wlandiag.o wlanfix.o vramcrc.o nvcopy.o vcn.o fw.o plugin_start.o info.o
KEXT   := $(NAME).kext/Contents
# The VBIOS is read at boot from this laptop's ACPI VFCT. A local build may also embed the augmented image as a
# fallback (Lenovo/AMD firmware, never committed or shipped): put it at fw/lenovo_660m-73ff-aug.rom, or pass
# VBIOS_ROM= (empty) to build without it.
VBIOS_ROM ?= $(wildcard fw/lenovo_660m-73ff-aug.rom)
ifneq ($(strip $(VBIOS_ROM)),)
OBJS   += vbios.o
CXXFLAGS += -DRMB_EMBED_VBIOS=1
endif

all: $(KEXT)/MacOS/$(NAME)

%.o: %.cpp rmb.hpp vbios_augment.hpp dispatch.hpp tahoe_hooks.hpp tahoe_windows.hpp tahoe_symbols_25G229.hpp
	clang++ $(CXXFLAGS) -MMD -MP -c $< -o $@
plugin_start.o: $(LILU)/Library/plugin_start.cpp
	clang++ $(CXXFLAGS) -MMD -MP -c $< -o $@
# GC/SDMA/DMUB/VCN firmware pieces, split from linux-firmware's yellow_carp_*.bin (not shipped in this repo).
FWSRC  ?= $(ROOT)/deps/yellow_carp_fw
firmware:
	python3 $(ROOT)/scripts/fw-extract.py "$(FWSRC)" fw
fw.o: fw.s firmware
	clang -target x86_64-apple-macos10.15 -c $< -o $@
vbios.o: vbios.s $(VBIOS_ROM)
	clang -target x86_64-apple-macos10.15 -c $< -o $@
info.o: info.c
	clang $(KFLAGS) -MMD -MP -c $< -o $@

$(NAME): $(OBJS) $(ROOT)/tools/strict_link.py
	rm -rf $(NAME).kext
	python3 $(ROOT)/tools/strict_link.py $@ $(DARWIN_LD) -arch x86_64 -kext -platform_version macos 10.15 10.15 -no_data_const $(OBJS) $(SDK)/Library/x86_64/libkmod.a

$(KEXT)/MacOS/$(NAME): $(NAME) Info.plist YELLOW_CARP_LICENSE.txt
	mkdir -p $(KEXT)/MacOS $(KEXT)/Resources && cp $(NAME) $@ && cp Info.plist $(KEXT)/Info.plist
	cp YELLOW_CARP_LICENSE.txt $(KEXT)/Resources/LICENSE-AMD-firmware.txt

# Every hook name must match the KDK, and every undefined symbol must come from the kernel or Lilu.
check:
	python3 -I $(ROOT)/tools/verify_symbol_manifest.py tahoe_symbols_25G229.hpp "$(KDK_EXTENSIONS)"
	python3 $(ROOT)/tools/check_imports.py $(KEXT)/MacOS/$(NAME) --kernel "$(KERNEL_INPUT)" --lilu "$(LILU_RUNTIME)"

clean:
	rm -rf *.o *.d $(NAME) $(NAME).kext fw fw.tmp

.PHONY: all check clean firmware

-include $(wildcard *.d)
