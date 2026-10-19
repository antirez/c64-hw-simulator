# c64-hw-sim - pin level simulator of the C64 ASSY 250407 motherboard.
#
#   make            build ./c64-hw-sim
#   make clean

CC      ?= cc
OPT     ?= -O3 -flto
CFLAGS  := $(OPT) -g -Wall -Wextra -Wno-unused-parameter -Isrc -Ichips -Ithird_party -DSOKOL_GLCORE

SIM_SRC = src/sim.c src/parts.c src/board.c src/cart.c src/harness.c src/board_data.c src/png.c src/chips_impl.c
APP_SRC = src/main.c src/ui.c src/inspect.c

UNAME := $(shell uname -s)
ifeq ($(UNAME),Darwin)
  SOKOL_FLAGS = -x objective-c -Wno-deprecated-declarations -Wno-unused-but-set-variable \
                -Wno-sign-compare -Wno-unused-variable
  LIBS = -framework Cocoa -framework QuartzCore -framework OpenGL
else
  SOKOL_FLAGS = -Wno-unused-but-set-variable -Wno-sign-compare -Wno-unused-variable
  LIBS = -lX11 -lXi -lXcursor -lGL -ldl -lpthread -lm
endif

all: c64-hw-sim

build/sokol_impl.o: src/sokol_impl.c
	@mkdir -p build
	$(CC) $(CFLAGS) $(SOKOL_FLAGS) -c $< -o $@

build/%.o: src/%.c src/*.h chips/*.h
	@mkdir -p build
	$(CC) $(CFLAGS) -c $< -o $@

OBJS = $(patsubst src/%.c,build/%.o,$(SIM_SRC) $(APP_SRC)) build/sokol_impl.o

c64-hw-sim: $(OBJS)
	$(CC) $(CFLAGS) $(OBJS) -o $@ $(LIBS)

clean:
	rm -rf build c64-hw-sim

.PHONY: all clean
