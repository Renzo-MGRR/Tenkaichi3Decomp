.SUFFIXES:
.DEFAULT_GOAL := all

PS3DEV ?= /usr/local/ps3dev
PSL1GHT ?= $(PS3DEV)
export PS3DEV PSL1GHT

PS3_PORT_DIR := $(abspath $(dir $(lastword $(MAKEFILE_LIST))))
ROOT := $(abspath $(PS3_PORT_DIR)/../..)
BUILD_DIR := $(ROOT)/port/build/ps3

# PSL1GHT sample-style output and intermediate variables.
TARGET := bt3
BUILD := $(BUILD_DIR)/psl1ght
OUTPUT := $(BUILD_DIR)/$(TARGET)
BUILDDIR := $(BUILD)

EBOOT := $(BUILD_DIR)/EBOOT.BIN

include $(PSL1GHT)/ppu_rules

.PHONY: all clean force-rebuild
all: $(OUTPUT).self $(EBOOT)

# The custom source translator emits OUTPUT.elf and the data sidecar.
# Re-run it because the generator does not yet emit complete Make dependencies.
$(OUTPUT).elf: force-rebuild
	cd $(ROOT) && bash port/ps3/build.sh

# ppu_rules supplies the standard %.self: %.elf strip/sprxlinker/make_self pipeline.
$(EBOOT): $(OUTPUT).self
	cp $< $@

force-rebuild:

clean:
	rm -rf $(ROOT)/port/build
