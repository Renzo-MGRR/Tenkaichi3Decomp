#!/bin/bash
# BT3 PS3 environment: call with `bash port/ps3/env.sh` from the repo top, or source it.
export PS3DEV=/usr/local/ps3dev
export PSL1GHT=/usr/local/ps3dev
export PATH="$PS3DEV/bin:$PS3DEV/ppu/bin:$PATH"
export OBJ=port/build/obj_ps3
export LOG=port/build/ps3
