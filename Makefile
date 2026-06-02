# Open LabBench — SDL2 application
#
# Build deps:
#   Ubuntu/Debian:  libsdl2-dev libsdl2-ttf-dev
#   Fedora:         SDL2-devel SDL2_ttf-devel
#   Arch:           sdl2 sdl2_ttf
#   Windows (MSYS2 MINGW64):
#                   pacman -S mingw-w64-x86_64-{gcc,SDL2,SDL2_ttf,pkg-config}
#
# Common invocations:
#   make                       # build psu_app, psu_probe, and legacy GUIs (POSIX only)
#   make app                   # only the new single-binary app
#   make probe                 # only the CLI sanity tool
#   make legacy                # only the four legacy GUIs (POSIX only)
#   make run                   # build + run psu_app
#   make run ARGS="--driver=modbus-bridge --view=toolbar-single --port=/dev/ttyUSB0"
#   make clean                 # remove build/ and all binaries
#   make install               # install psu_app to $(PREFIX)/bin  (POSIX only)

CC      ?= gcc
PREFIX  ?= /usr/local
BINDIR  ?= $(PREFIX)/bin
BUILD   ?= build

# ----- platform detection -------------------------------------------------

# OS=Windows_NT is set by cmd.exe / MSYS2's make. uname -s also identifies the host.
ifeq ($(OS),Windows_NT)
    PLATFORM      := windows
    EXE_SUFFIX    := .exe
    PLATFORM_SRC  := src/platform/platform_win32.c
    SERIAL_SRC    := src/transport/serial_port_win32.c
    PLATFORM_LIBS := -lwinmm -lws2_32 -lkernel32 -lgdi32 -limm32 -lole32 -loleaut32 -luuid -lsetupapi -lversion
    BUILD_LEGACY  := 0
else
    UNAME_S := $(shell uname -s)
    PLATFORM      := posix
    EXE_SUFFIX    :=
    PLATFORM_SRC  := src/platform/platform_posix.c
    SERIAL_SRC    := src/transport/serial_port.c
    PLATFORM_LIBS :=
    BUILD_LEGACY  := 1
endif

# ----- SDL2 detection -----------------------------------------------------

# pkg-config works on Linux + macOS + Windows-under-MSYS2; the -l fallback
# handles barebones systems without pkg-config.
SDL_CFLAGS := $(shell pkg-config --cflags sdl2 SDL2_ttf 2>/dev/null)
SDL_LIBS   := $(shell pkg-config --libs   sdl2 SDL2_ttf 2>/dev/null)
ifeq ($(strip $(SDL_LIBS)),)
SDL_LIBS := -lSDL2 -lSDL2_ttf
endif

# ----- libusb detection (optional — enables userspace USB-TMC) -----------

# When libusb-1.0 is present (Linux: libusb-1.0-0-dev; MSYS2:
# mingw-w64-x86_64-libusb), the userspace USB-TMC backend gets compiled in.
# Without it the kernel-driver path on Linux still works; Windows users
# need libusb for any USB-TMC access.
USB_CFLAGS := $(shell pkg-config --cflags libusb-1.0 2>/dev/null)
USB_LIBS   := $(shell pkg-config --libs   libusb-1.0 2>/dev/null)
ifneq ($(strip $(USB_LIBS)),)
USB_CFLAGS += -DHAVE_LIBUSB
endif

# ----- compile / link flags ----------------------------------------------

NEW_INCLUDES := -Iinclude -Isrc -Isrc/transport

CFLAGS  ?= -O2
CFLAGS  += -Wall -Wextra -std=c99 -pthread
ifeq ($(PLATFORM),posix)
CFLAGS  += -D_POSIX_C_SOURCE=200809L -D_DEFAULT_SOURCE
endif

# SDL_CFLAGS is applied per-binary (see *_CPPFLAGS below) so it isn't
# inherited by binaries that don't link SDL — on Windows it carries the
# `-Dmain=SDL_main` macro that would otherwise rename psu_probe's main()
# and leave the linker without a WinMain.

LDFLAGS += -pthread
LDLIBS  += $(SDL_LIBS) $(PLATFORM_LIBS) $(USB_LIBS) -lm

# ----- source lists -------------------------------------------------------

TRANSPORT_SRCS := \
    $(SERIAL_SRC) \
    src/transport/scpi.c \
    src/transport/scpi_serial.c \
    src/transport/scpi_prologix.c \
    src/transport/scpi_usbtmc.c \
    src/transport/scpi_vxi11.c \
    src/transport/scpi_hislip.c \
    src/transport/net_io.c

DRIVER_SRCS := \
    src/drivers/registry.c \
    src/drivers/demo.c \
    src/drivers/dmm_demo.c \
    src/drivers/dmm_helpers.c \
    src/drivers/modbus_bridge/modbus_bridge.c \
    src/drivers/modbus_bridge/psu_protocol.c \
    src/drivers/scpi_psu/scpi_psu.c \
    src/drivers/korad/korad.c \
    src/drivers/owon_xdm/owon_xdm.c \
    src/drivers/scpi_dmm/scpi_dmm.c \
    src/drivers/hp_3458a/hp_3458a.c \
    src/drivers/hp_3478a/hp_3478a.c \
    src/drivers/hp_6620_family/hp_6620_family.c

VIEW_SRCS := \
    src/views/registry.c \
    src/views/vfd_dotmatrix.c \
    src/views/toolbar_single.c \
    src/views/toolbar_dual.c \
    src/views/full_common.c \
    src/views/full_dual.c \
    src/views/full_single.c \
    src/views/dmm_toolbar.c \
    src/views/dmm_full.c

# Legacy binaries link directly against POSIX termios; skipped on Windows.

# ----- binaries -----------------------------------------------------------

APP_BINS    := psu_app$(EXE_SUFFIX)
TOOL_BINS   := psu_probe$(EXE_SUFFIX)
ifeq ($(BUILD_LEGACY),1)
LEGACY_BINS := psu_gui psu_gui_single psu_gui_toolbar psu_gui_toolbar_single
else
LEGACY_BINS :=
endif
BINS := $(APP_BINS) $(TOOL_BINS) $(LEGACY_BINS)

# Per-binary source lists.
psu_app$(EXE_SUFFIX)_SRCS := \
    src/app/psu_app.c src/app/launcher.c \
    $(PLATFORM_SRC) $(TRANSPORT_SRCS) $(DRIVER_SRCS) $(VIEW_SRCS)

psu_probe$(EXE_SUFFIX)_SRCS := \
    src/app/psu_probe.c $(PLATFORM_SRC) $(TRANSPORT_SRCS) $(DRIVER_SRCS)

# psu_probe doesn't link SDL/TTF — keep SDL_CFLAGS off its compile line.
psu_probe$(EXE_SUFFIX)_LDLIBS   := -pthread $(PLATFORM_LIBS) $(USB_LIBS) -lm
psu_probe$(EXE_SUFFIX)_CPPFLAGS := $(NEW_INCLUDES) $(USB_CFLAGS)

# psu_app does need SDL; pulled in via CPPFLAGS (not the global CFLAGS).
psu_app$(EXE_SUFFIX)_CPPFLAGS   := $(NEW_INCLUDES) $(SDL_CFLAGS) $(USB_CFLAGS)

# Legacy four GUIs (POSIX only).
LEGACY_INC := -Ilegacy

psu_gui_SRCS                := legacy/main.c                legacy/serial_port.c legacy/psu_protocol.c
psu_gui_single_SRCS         := legacy/main_single.c         legacy/serial_port.c legacy/psu_protocol.c
psu_gui_toolbar_SRCS        := legacy/main_toolbar.c        legacy/serial_port.c legacy/psu_protocol.c
psu_gui_toolbar_single_SRCS := legacy/main_toolbar_single.c legacy/serial_port.c legacy/psu_protocol.c

psu_gui_CPPFLAGS                := $(LEGACY_INC) $(SDL_CFLAGS)
psu_gui_single_CPPFLAGS         := $(LEGACY_INC) $(SDL_CFLAGS)
psu_gui_toolbar_CPPFLAGS        := $(LEGACY_INC) $(SDL_CFLAGS)
psu_gui_toolbar_single_CPPFLAGS := $(LEGACY_INC) $(SDL_CFLAGS)

# ----- build rules --------------------------------------------------------

.PHONY: all app probe legacy clean install uninstall run platform
.DEFAULT_GOAL := all

all:    $(BINS)
app:    $(APP_BINS)
probe:  $(TOOL_BINS)
legacy: $(LEGACY_BINS)

platform:
	@echo "Building for: $(PLATFORM)"
	@echo "  exe suffix:   '$(EXE_SUFFIX)'"
	@echo "  platform src: $(PLATFORM_SRC)"
	@echo "  serial src:   $(SERIAL_SRC)"
	@echo "  extra libs:   $(PLATFORM_LIBS)"
	@echo "  build legacy: $(BUILD_LEGACY)"

define BUILD_template
$(1)_OBJS := $$(patsubst %.c,$(BUILD)/$(1)/%.o,$$($(1)_SRCS))

$$($(1)_OBJS): $(BUILD)/$(1)/%.o: %.c
	@mkdir -p $$(@D)
	$$(CC) $$($(1)_CPPFLAGS) $$(CFLAGS) -c $$< -o $$@

$(1): $$($(1)_OBJS)
	$$(CC) $$(LDFLAGS) $$^ -o $$@ $$(if $$($(1)_LDLIBS),$$($(1)_LDLIBS),$$(LDLIBS))
endef
$(foreach bin,$(BINS),$(eval $(call BUILD_template,$(bin))))

run: $(APP_BINS)
	./$(APP_BINS) $(if $(ARGS),$(ARGS),--driver=demo --view=toolbar-single --port=-)

install: $(APP_BINS)
	install -d $(DESTDIR)$(BINDIR)
	install -m 755 $(APP_BINS) $(DESTDIR)$(BINDIR)/

uninstall:
	cd $(DESTDIR)$(BINDIR) && rm -f $(APP_BINS)

clean:
	rm -rf $(BUILD) $(BINS) psu_app.exe psu_probe.exe
