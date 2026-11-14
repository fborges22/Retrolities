# Native x64 Windows build for Picasso, using MinGW-w64.

ROOT := ../..
BUILD := $(ROOT)/build/win64
OBJ_DIR := $(BUILD)/obj/picasso
BIN_DIR := $(BUILD)/bin

MINGW_PREFIX ?= x86_64-w64-mingw32-
W64_CC ?= $(MINGW_PREFIX)gcc
W64_RC ?= $(MINGW_PREFIX)windres
CC := $(W64_CC)
RC := $(W64_RC)

CPPFLAGS := -DRETRO_WIN64 -DRETRO_BITS=64 -DRETRO_GUI \
            -DWINVER=0x0601 -D_WIN32_WINNT=0x0601 \
            -I. -I$(ROOT)/lib/retro -I$(ROOT)/lib/imgio
CFLAGS := -O2 -Wall -Wextra
LDLIBS := -lcomdlg32 -lgdi32 -lshell32 -luser32

SOURCES := $(wildcard $(ROOT)/src/picasso/*.c) \
           $(wildcard $(ROOT)/lib/imgio/*.c) \
           $(ROOT)/lib/retro/platform.c
OBJECTS := $(patsubst $(ROOT)/%.c,$(OBJ_DIR)/%.o,$(SOURCES))
RESOURCE := $(OBJ_DIR)/picasso.res
TARGET := $(BIN_DIR)/PICASSO64.EXE

.PHONY: all win64 clean
all: win64

win64: $(TARGET)

$(OBJ_DIR)/%.o: $(ROOT)/%.c
	@mkdir -p $(@D)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c -o $@ $<

$(RESOURCE): picasso.rc resource.h $(wildcard res/*)
	@mkdir -p $(@D)
	$(RC) -DRETRO_WIN64 -I. -O coff -o $@ $<

$(TARGET): $(OBJECTS) $(RESOURCE)
	@mkdir -p $(BIN_DIR)
	$(CC) -mwindows -static -o $@ $(OBJECTS) $(RESOURCE) $(LDLIBS)

clean:
	rm -rf $(OBJ_DIR) $(TARGET)
