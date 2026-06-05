# Open LabBench — SDL2 + Dear ImGui application
#
# Build deps:
#   Ubuntu/Debian:  libsdl2-dev libsdl2-ttf-dev libgl1-mesa-dev
#                   (+ libusb-1.0-0-dev for USB-TMC)
#   Fedora:         SDL2-devel SDL2_ttf-devel mesa-libGL-devel
#                   (+ libusbx-devel for USB-TMC)
#   Arch:           sdl2 sdl2_ttf mesa
#                   (+ libusb for USB-TMC)
#   Windows (MSYS2 MINGW64):
#                   pacman -S mingw-w64-x86_64-{gcc,SDL2,SDL2_ttf,libusb,pkg-config}
#
# Common invocations:
#   make                       # build psu_app + psu_probe
#   make app                   # only psu_app
#   make probe                 # only psu_probe (CLI driver sanity tool)
#   make run                   # build + run psu_app
#   make run ARGS="--driver=modbus-bridge --view=toolbar-single --port=/dev/ttyUSB0"
#   make clean                 # remove build/ and all binaries
#   make install               # install psu_app to $(PREFIX)/bin

CC      ?= gcc
CXX     ?= g++
PREFIX  ?= /usr/local
BINDIR  ?= $(PREFIX)/bin
BUILD   ?= build

# ----- platform detection -------------------------------------------------

ifeq ($(OS),Windows_NT)
    PLATFORM      := windows
    EXE_SUFFIX    := .exe
    PLATFORM_SRC  := src/platform/platform_win32.c
    SERIAL_SRC    := src/transport/serial_port_win32.c
    PLATFORM_LIBS := -lwinmm -lws2_32 -lkernel32 -lgdi32 -limm32 -lole32 -loleaut32 -luuid -lsetupapi -lversion
else
    UNAME_S := $(shell uname -s)
    PLATFORM      := posix
    EXE_SUFFIX    :=
    PLATFORM_SRC  := src/platform/platform_posix.c
    SERIAL_SRC    := src/transport/serial_port.c
    PLATFORM_LIBS :=
endif

# ----- SDL2 detection -----------------------------------------------------

SDL_CFLAGS := $(shell pkg-config --cflags sdl2 SDL2_ttf 2>/dev/null)
SDL_LIBS   := $(shell pkg-config --libs   sdl2 SDL2_ttf 2>/dev/null)
ifeq ($(strip $(SDL_LIBS)),)
SDL_LIBS := -lSDL2 -lSDL2_ttf
endif

# ----- libusb detection (optional — enables userspace USB-TMC) -----------

USB_CFLAGS := $(shell pkg-config --cflags libusb-1.0 2>/dev/null)
USB_LIBS   := $(shell pkg-config --libs   libusb-1.0 2>/dev/null)
ifneq ($(strip $(USB_LIBS)),)
USB_CFLAGS += -DHAVE_LIBUSB
endif

# OpenGL link for the ImGui renderer backend.
ifeq ($(PLATFORM),posix)
    GL_LIBS := -lGL -ldl
else
    GL_LIBS := -lopengl32
endif

# ----- compile / link flags ----------------------------------------------

NEW_INCLUDES   := -Iinclude -Isrc -Isrc/transport
IMGUI_INCLUDES := -Ithird_party/imgui -Ithird_party/imgui/backends

CFLAGS  ?= -O2
CFLAGS  += -Wall -Wextra -std=c99 -pthread
ifeq ($(PLATFORM),posix)
CFLAGS  += -D_POSIX_C_SOURCE=200809L -D_DEFAULT_SOURCE
endif

# C++ compile flags for ImGui + the shell. ImGui builds clean without
# exceptions / RTTI.
CXXFLAGS ?= -O2
CXXFLAGS += -Wall -Wextra -std=c++17 -pthread -fno-exceptions -fno-rtti

LDFLAGS += -pthread
LDLIBS  += $(SDL_LIBS) $(GL_LIBS) $(PLATFORM_LIBS) $(USB_LIBS) -lm

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

# Plain C view-registry only — the rendering is in the ImGui shell.
VIEW_SRCS := src/views/registry.c

# Dear ImGui (docking branch) — vendored under third_party/imgui/.
IMGUI_SRCS := \
    third_party/imgui/imgui.cpp \
    third_party/imgui/imgui_draw.cpp \
    third_party/imgui/imgui_tables.cpp \
    third_party/imgui/imgui_widgets.cpp \
    third_party/imgui/imgui_demo.cpp \
    third_party/imgui/backends/imgui_impl_sdl2.cpp \
    third_party/imgui/backends/imgui_impl_opengl3.cpp

SHELL_SRCS := \
    src/shell/shell.cpp \
    src/shell/launcher_imgui.cpp \
    src/shell/instance.cpp \
    src/shell/views_imgui/widgets.cpp \
    src/shell/views_imgui/toolbar_single.cpp \
    src/shell/views_imgui/toolbar_dual.cpp \
    src/shell/views_imgui/full_single.cpp \
    src/shell/views_imgui/full_dual.cpp \
    src/shell/views_imgui/dmm_toolbar.cpp \
    src/shell/views_imgui/dmm_full.cpp

# ----- binaries -----------------------------------------------------------

APP_BINS  := psu_app$(EXE_SUFFIX)
TOOL_BINS := psu_probe$(EXE_SUFFIX)
BINS := $(APP_BINS) $(TOOL_BINS)

psu_app$(EXE_SUFFIX)_SRCS := \
    src/app/psu_app.c \
    $(PLATFORM_SRC) $(TRANSPORT_SRCS) $(DRIVER_SRCS) $(VIEW_SRCS) \
    $(IMGUI_SRCS) $(SHELL_SRCS)

psu_probe$(EXE_SUFFIX)_SRCS := \
    src/app/psu_probe.c $(PLATFORM_SRC) $(TRANSPORT_SRCS) $(DRIVER_SRCS)

# psu_probe doesn't link SDL/TTF/ImGui — keep their CFLAGS off its compiles.
psu_probe$(EXE_SUFFIX)_LDLIBS   := -pthread $(PLATFORM_LIBS) $(USB_LIBS) -lm
psu_probe$(EXE_SUFFIX)_CPPFLAGS := $(NEW_INCLUDES) $(USB_CFLAGS)

# psu_app: SDL + ImGui pulled in via CPPFLAGS (not the global CFLAGS, so
# the SDL2 -Dmain=SDL_main macro doesn't escape into psu_probe).
psu_app$(EXE_SUFFIX)_CPPFLAGS := $(NEW_INCLUDES) $(IMGUI_INCLUDES) $(SDL_CFLAGS) $(USB_CFLAGS)

# Final link goes through g++ so libstdc++ comes in automatically.
psu_app$(EXE_SUFFIX)_LINK := $(CXX)

# ----- build rules --------------------------------------------------------

.PHONY: all app probe clean install uninstall run platform
.DEFAULT_GOAL := all

all:   $(BINS)
app:   $(APP_BINS)
probe: $(TOOL_BINS)

platform:
	@echo "Building for: $(PLATFORM)"
	@echo "  exe suffix:   '$(EXE_SUFFIX)'"
	@echo "  platform src: $(PLATFORM_SRC)"
	@echo "  serial src:   $(SERIAL_SRC)"
	@echo "  extra libs:   $(PLATFORM_LIBS)"
	@echo "  libusb:       $(if $(USB_LIBS),yes,no)"

define BUILD_template
# Split per-binary sources by language so each gets the right compiler.
$(1)_C_SRCS   := $$(filter %.c,$$($(1)_SRCS))
$(1)_CXX_SRCS := $$(filter %.cpp,$$($(1)_SRCS))
$(1)_OBJS     := $$(patsubst %.c,$(BUILD)/$(1)/%.o,$$($(1)_C_SRCS)) \
                 $$(patsubst %.cpp,$(BUILD)/$(1)/%.o,$$($(1)_CXX_SRCS))

$$(patsubst %.c,$(BUILD)/$(1)/%.o,$$($(1)_C_SRCS)): $(BUILD)/$(1)/%.o: %.c
	@mkdir -p $$(@D)
	$$(CC) $$($(1)_CPPFLAGS) $$(CFLAGS) -c $$< -o $$@

$$(patsubst %.cpp,$(BUILD)/$(1)/%.o,$$($(1)_CXX_SRCS)): $(BUILD)/$(1)/%.o: %.cpp
	@mkdir -p $$(@D)
	$$(CXX) $$($(1)_CPPFLAGS) $$(CXXFLAGS) -c $$< -o $$@

$(1): $$($(1)_OBJS)
	$$(if $$($(1)_LINK),$$($(1)_LINK),$$(CC)) \
	    $$(LDFLAGS) $$^ -o $$@ \
	    $$(if $$($(1)_LDLIBS),$$($(1)_LDLIBS),$$(LDLIBS))
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
