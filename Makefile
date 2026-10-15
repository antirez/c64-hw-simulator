# c64-hw-sim - pin level simulator of the C64 ASSY 250407 motherboard.
#
#   make            build ./c64-hw-sim
#   make C64HW_WAYLAND=1    native Wayland build (X11 is the default)
#   make clean

CC      ?= cc
OPT     ?= -O3 -flto

SIM_SRC = src/sim.c src/parts.c src/board.c src/board_data.c src/png.c src/chips_impl.c
APP_SRC = src/main.c src/ui.c

UNAME := $(shell uname -s)
ifeq ($(UNAME),Darwin)
  CFLAGS  := $(OPT) -g -Wall -Wextra -Wno-unused-parameter -Isrc -Ichips -Ithird_party -DSOKOL_GLCORE
  SOKOL_FLAGS = -x objective-c -Wno-deprecated-declarations -Wno-unused-but-set-variable \
                -Wno-sign-compare -Wno-unused-variable
  LIBS = -framework Cocoa -framework QuartzCore -framework OpenGL
else ifeq ($(C64HW_WAYLAND),1)
  CFLAGS  := $(OPT) -g -Wall -Wextra -Wno-unused-parameter -Isrc -Ichips -Ithird_party \
             -DSOKOL_GLCORE -DSOKOL_WAYLAND \
             $(shell pkg-config --cflags wayland-client wayland-cursor xkbcommon)
  SOKOL_FLAGS = -Wno-unused-but-set-variable -Wno-sign-compare -Wno-unused-variable
  LIBS = $(shell pkg-config --libs wayland-client wayland-cursor xkbcommon) -lEGL -lGL -ldl -lpthread -lm
else
  CFLAGS  := $(OPT) -g -Wall -Wextra -Wno-unused-parameter -Isrc -Ichips -Ithird_party -DSOKOL_GLCORE
  SOKOL_FLAGS = -Wno-unused-but-set-variable -Wno-sign-compare -Wno-unused-variable
  LIBS = -lX11 -lXi -lXcursor -lGL -ldl -lpthread -lm
endif

all: c64-hw-sim

build/sokol_impl.o: src/sokol_impl.c
	@mkdir -p build
	$(CC) $(CFLAGS) $(SOKOL_FLAGS) -c $< -o $@

build/xdg-shell-protocol.o: third_party/wayland/xdg-shell-protocol.c
	@mkdir -p build
	$(CC) $(CFLAGS) -c $< -o $@

build/xdg-decoration-protocol.o: third_party/wayland/xdg-decoration-unstable-v1-protocol.c
	@mkdir -p build
	$(CC) $(CFLAGS) -c $< -o $@

build/%.o: src/%.c src/*.h chips/*.h
	@mkdir -p build
	$(CC) $(CFLAGS) -c $< -o $@

ifeq ($(C64HW_WAYLAND),1)
OBJS = $(patsubst src/%.c,build/%.o,$(SIM_SRC) $(APP_SRC)) build/sokol_impl.o \
       build/xdg-shell-protocol.o build/xdg-decoration-protocol.o
else
OBJS = $(patsubst src/%.c,build/%.o,$(SIM_SRC) $(APP_SRC)) build/sokol_impl.o
endif

c64-hw-sim: $(OBJS)
	$(CC) $(CFLAGS) $(OBJS) -o $@ $(LIBS)

clean:
	rm -rf build c64-hw-sim

.PHONY: all clean