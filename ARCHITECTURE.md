# Architecture

The app is split so each PSU model is a *driver* and each layout is a *view*.
Any view can run against any driver, and a single binary can launch any
combination — including several instances in parallel (the launcher
fork/execs `psu_app` per window).

## Layout

```text
include/
  psu_driver.h                    Public PSU driver interface — PSU views talk only to this.
  dmm_driver.h                    Public DMM driver interface — DMM views talk only to this.

src/
  app/
    psu_app.c                     Entry point. No args → launcher; with --driver/--view → run that combination.
    launcher.{c,h}                SDL launcher window: pick driver/view/port, LAUNCH spawns new instance.
    psu_probe.c                   Non-SDL CLI for sanity-checking drivers without launching the GUI.

  drivers/
    registry.{c,h}                List of compiled-in driver factories (both PSU + DMM) —
                                  exports psu_drivers_list/find and dmm_drivers_list/find.
    demo.{c,h}                    Synthetic 2-channel PSU (no hardware required).
    dmm_demo.{c,h}                Synthetic 6½-digit DMM.
    dmm_helpers.c                 Tiny shared helpers — mode label / unit lookup.
    modbus_bridge/                ESP32 Modbus-text-bridge driver (Riden RD60xx etc.).
      modbus_bridge.{c,h}
      psu_protocol.{c,h}          Wire protocol used only by this driver.
    scpi_psu/                     One PSU driver, many profiles. Currently shipped:
      scpi_psu.{c,h}                Siglent SPD3303; Keysight E3631A/3A/4A/5A;
                                    Rigol DP832/832A/811/711; R&S HMP4040/4030/2030/NGE103B;
                                    Keithley 2230G/2231A; classic HP 6632A/6633A/6634A.
                                  Works over either USB-serial or Prologix GPIB transport.
    korad/                        Korad KA-protocol PSU driver — Korad / TENMA / Velleman / Hanmatek / clones.
      korad.{c,h}
    hp_3458a/                     HP DCL DMM core — profile-driven. Ships hp-3458a
      hp_3458a.{c,h}                (8½-digit reference) + hp-3457a (6½-digit predecessor).
                                  DCV/ACV/OHM/OHMF + TARM SGL trigger model. GPIB-only.
    hp_3478a/                     HP 3478A — 5½-digit DMM, single-letter F-command
      hp_3478a.{c,h}                protocol (F1=DCV, R5=range, T3=single trigger). GPIB-only.
    hp_6620_family/               HP 6620-series System DC Sources — native HP-IB DCL,
      hp_6620_family.{c,h}          NOT SCPI. Ships hp-6622a / 6623a / 6624a / 6625a / 6627a
                                  factories. VSET/ISET/OUT/VOUT?/IOUT? command set. GPIB-only.
    owon_xdm/                     OWON XDM-series DMM driver — SCPI over USB serial.
      owon_xdm.{c,h}              Supports XDM1041/1241/2041 (+XDM3000 if SCPI-compatible).
    scpi_dmm/                     One DMM driver, many SCPI profiles. Currently shipped:
      scpi_dmm.{c,h}                Keysight/Agilent/HP 34401A; Keysight Truevolt
                                    34461A / 34465A / 34470A; Fluke 8845A / 8846A
                                    (34401A-compat); Keithley 2000 and DMM6500.
                                  Same scpi.h transport as scpi_psu — every model
                                  reaches the instrument over either USB-serial or
                                  Prologix GPIB-USB-HPIB.

  platform/                       Thin OS shim — keeps everything else portable.
    platform.h                    Public API: pl_sleep_ms, pl_now_ms, pl_self_exe, pl_spawn.
    platform_posix.c              POSIX backend (Linux/macOS): usleep, gettimeofday,
                                  /proc/self/exe, fork+execv, SIGCHLD ignore.
    platform_win32.c              Win32 backend: Sleep, GetTickCount64,
                                  GetModuleFileNameW, CreateProcessW.

  transport/                      Wire layers shared by drivers.
    serial_port.{c,h}             POSIX serial helper (termios).
    serial_port_win32.c           Native Win32 alternative (CreateFile + SetCommState +
                                  ReadFile/WriteFile). Makefile picks one per OS.
    scpi.{c,h}                    SCPI client API + port-spec parser. Schemes:
                                    serial: / prologix: / usbtmc: / vxi11: / hislip:
    scpi_serial.c                 SCPI transport: direct USB-serial.
    scpi_prologix.c               SCPI transport: Prologix GPIB-USB-HPIB controller.
    scpi_usbtmc.c                 SCPI transport: USB-TMC. Linux uses /dev/usbtmcN via
                                    the kernel `usbtmc` module; libusb-1.0 path
                                    handles vid:pid form (cross-platform).
    scpi_vxi11.c                  SCPI transport: VXI-11 over TCP. Hand-rolled ONC RPC /
                                    XDR (no external dep) — portmap GETPORT → CREATE_LINK
                                    → DEVICE_WRITE/READ → DESTROY_LINK.
    scpi_hislip.c                 SCPI transport: HiSLIP IVI-6.1 binary protocol on
                                    TCP 4880. v1: Initialize + Data/DataEnd only.
    net_io.{c,h}                  Tiny cross-platform TCP helper (Linux/POSIX +
                                    Winsock2) used by scpi_vxi11.c and scpi_hislip.c.

  views/
    views.h                       view_def_t + dmm_view_def_t + entry-point declarations.
    registry.c                    Lists of compiled-in PSU views and DMM views.
    toolbar_single.c              PSU compact 1-channel strip.
    toolbar_dual.c                PSU compact 2-channel strip.
    full_common.{c,h}             Shared PSU toolkit (VFD readouts, bar/temp/scope, keypad, …).
    full_single.c                 PSU full GUI — single channel.
    full_dual.c                   PSU full GUI — dual channel.
    dmm_toolbar.c                 DMM compact strip readout.
    dmm_full.c                    DMM full GUI — mode/range/rate buttons + statistics + trace.

legacy/                           The original four standalone PSU GUIs, kept building during the
                                  refactor as a side-by-side reference. Removed once the new
                                  full views are validated on hardware.

screenshots/  Makefile  README.md  LICENSE  .gitignore
```

## Layering

```text
   views   ──depend on──>  psu_driver.h          ◄── only public interface
                                ▲
                                │ implemented by
                                │
   drivers ──own their own── wire encoding (e.g. drivers/modbus_bridge/psu_protocol.c,
                                drivers/scpi_psu/scpi_psu.c, drivers/korad/korad.c)
                                │
                                ▼
                          transport/ (serial_port, scpi*)
```

Rules:

- Views never include driver or transport headers.
- Drivers never include view or app headers.
- The registry is the only place that knows the full list of drivers — adding
  a model is one line in `src/drivers/registry.c` + a new sibling folder (or
  one extra entry in `scpi_psu/scpi_psu.c` if it's another SCPI instrument).

## Port-spec grammar (SCPI drivers)

SCPI-class drivers (Siglent, Keysight) accept any of these `--port=...` formats:

```text
serial:/dev/ttyUSB0                 # direct USB-serial (default baud from factory)
serial:/dev/ttyUSB0:9600            # with explicit baud override
prologix:/dev/ttyUSB0:5             # Prologix GPIB-USB-HPIB controller, GPIB addr 5
prologix:/dev/ttyUSB0:5:115200      # with explicit controller baud
/dev/ttyUSB0                        # shorthand → serial:/dev/ttyUSB0
```

That same `--port=prologix:...` form lets any compiled-in Keysight/Agilent/HP
profile reach instruments living on a GPIB bus, since the transport is the
only thing that changes.

## How to add a new PSU model

**SCPI instrument** (Siglent / Keysight / Agilent / HP / any other SCPI PSU):

1. Add a `scpi_psu_profile_t` entry in `src/drivers/scpi_psu/scpi_psu.c` —
   one struct with the model's command formats and channel count.
2. Define a thin `static psu_driver_t *open_<model>(...)` that calls
   `scpi_psu_open(&k_<model>, ...)`.
3. Add a `psu_driver_factory_t <model>_factory` and declare it in
   `scpi_psu.h`.
4. Register it in `src/drivers/registry.c` and add the file to `DRIVER_SRCS`
   in the Makefile (already there for scpi_psu).

**Non-SCPI instrument** (custom wire protocol):

1. Create `src/drivers/<my_model>/<my_model>.{c,h}` implementing the
   `psu_driver_t` vtable + a factory.
2. Add it to `k_drivers[]` in `src/drivers/registry.c`.
3. Add its source to `DRIVER_SRCS` in the Makefile.

Every view picks it up automatically.

## How to add a new view layout

1. Create `src/views/<my_view>.c` exposing `int view_<my_view>_run(psu_driver_t *);`.
2. Declare its entry point in `src/views/views.h`.
3. Add an entry to `k_views[]` in `src/views/registry.c`.
4. Add its source to `VIEW_SRCS` in the Makefile.

The launcher discovers it automatically and greys it out for drivers whose
`n_channels_hint` is below the view's `min_channels`.
