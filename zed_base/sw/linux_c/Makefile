# zed_base jig on Linux (PetaLinux 2024.2 kernel + Ubuntu 22.04), built natively on the board.
# Builds the bare-metal sources in ../common unchanged (jig.c selects its Linux parts with
# ZB_LINUX), the unmodified Xilinx drivers in xlnx/ and the BSP shim in shim/.
#   make && sudo ./jig
CC      ?= gcc
CXX     ?= g++
OPT     ?= -O2
COMMON  := ../common
WARN    := -Wall -Wno-unused-variable -Wno-unused-but-set-variable -Wno-unused-function \
           -Wno-format -Wno-format-truncation
# Bare-metal code and Xilinx drivers: standalone (SDT) code paths, Linux branches off
BMFLAGS := $(OPT) $(WARN) -U__linux__ -DSDT -DZB_LINUX -Ishim -Ixlnx -I$(COMMON)
LXFLAGS := $(OPT) $(WARN) -Ishim

XLNX := $(wildcard xlnx/*.c)
BMSRC := $(XLNX) shim/zb_xlnx_cfg.c $(COMMON)/zb_platform.c $(COMMON)/sw_kernels.c \
         $(COMMON)/lle_global.c $(COMMON)/jig.c
OBJ  := $(patsubst %.c,build/%.o,$(notdir $(BMSRC))) build/zb_linux.o build/zb_cv.o
CV_CFLAGS := $(shell pkg-config --cflags opencv4)
CV_LIBS   := $(shell pkg-config --libs opencv4)
vpath %.c xlnx shim $(COMMON)

jig: $(OBJ)
	$(CXX) -o $@ $^ -lgpiod $(CV_LIBS)

build/zb_cv.o: shim/zb_cv.cpp shim/zb_cv.h
	@mkdir -p build
	$(CXX) $(OPT) -Wall -Ishim $(CV_CFLAGS) -c $< -o $@

build/zb_linux.o: shim/zb_linux.c $(wildcard shim/*.h)
	@mkdir -p build
	$(CC) $(LXFLAGS) -c $< -o $@

build/%.o: %.c $(wildcard shim/*.h $(COMMON)/*.h)
	@mkdir -p build
	$(CC) $(BMFLAGS) -c $< -o $@

# shared library of the golden models for the Python jig (ctypes)
libzbmodels.so: $(COMMON)/sw_kernels.c $(COMMON)/lle_global.c
	$(CC) $(OPT) -shared -fPIC -U__linux__ -Ishim -I$(COMMON) $^ -o $@

clean:
	rm -rf build jig libzbmodels.so

.PHONY: clean
