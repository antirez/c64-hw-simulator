# c64-hw-sim - pin level simulator of the C64 ASSY 250407 motherboard.
#
#   make            build ./c64-hw-sim (c64-hw-sim.exe on Windows)
#   make clean

CC      ?= cc
OPT     ?= -O3 -flto
CFLAGS  := $(OPT) -g -Wall -Wextra -Wno-unused-parameter -Isrc -Ichips -Ithird_party -DSOKOL_GLCORE

SIM_SRC = src/sim.c src/parts.c src/board.c src/board_data.c src/png.c src/chips_impl.c
APP_SRC = src/main.c src/ui.c

EXE =
ifeq ($(OS),Windows_NT)
  # MinGW-w64 gcc; run make from a POSIX shell (Git Bash, MSYS2) for mkdir/rm.
  ifeq ($(origin CC),default)
    CC = gcc
  endif
  EXE = .exe
  SOKOL_FLAGS = -Wno-unused-but-set-variable -Wno-sign-compare -Wno-unused-variable \
                -Wno-cast-function-type -Wno-unknown-pragmas
  LIBS = -static -lkernel32 -luser32 -lgdi32 -lshell32 -lopengl32 -lm
else ifeq ($(shell uname -s),Darwin)
  SOKOL_FLAGS = -x objective-c -Wno-deprecated-declarations -Wno-unused-but-set-variable \
                -Wno-sign-compare -Wno-unused-variable
  LIBS = -framework Cocoa -framework QuartzCore -framework OpenGL
else
  SOKOL_FLAGS = -Wno-unused-but-set-variable -Wno-sign-compare -Wno-unused-variable
  LIBS = -lX11 -lXi -lXcursor -lGL -ldl -lpthread -lm
endif

all: c64-hw-sim$(EXE)

build/sokol_impl.o: src/sokol_impl.c third_party/sokol/*.h third_party/nuklear/*.h
	@mkdir -p build
	$(CC) $(CFLAGS) $(SOKOL_FLAGS) -c $< -o $@

build/%.o: src/%.c src/*.h chips/*.h
	@mkdir -p build
	$(CC) $(CFLAGS) -c $< -o $@

OBJS = $(patsubst src/%.c,build/%.o,$(SIM_SRC) $(APP_SRC)) build/sokol_impl.o

c64-hw-sim$(EXE): $(OBJS)
	$(CC) $(CFLAGS) $(OBJS) -o $@ $(LIBS)

clean:
	rm -rf build c64-hw-sim$(EXE)

.PHONY: all clean
