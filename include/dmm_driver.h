/**
 * dmm_driver.h - UI-facing DMM (digital multimeter) driver interface.
 *
 * Parallel to psu_driver.h but shaped around meter operations: one
 * measurement at a time, picked from a small set of modes, optionally
 * scaled by a range. Each DMM driver implements this vtable; DMM views
 * (dmm_toolbar.c, dmm_full.c) consume only this header.
 *
 * Thread-safety: drivers run their own reader thread internally and snapshot
 * state under a mutex. read() returns an independent copy.
 */

#ifndef DMM_DRIVER_H
#define DMM_DRIVER_H

#include <stdbool.h>
#include <stdint.h>

/* Measurement modes. Drivers expose which ones they support via caps. */
typedef enum {
    DMM_MODE_DC_VOLTS = 0,
    DMM_MODE_AC_VOLTS,
    DMM_MODE_DC_AMPS,
    DMM_MODE_AC_AMPS,
    DMM_MODE_OHMS_2W,
    DMM_MODE_OHMS_4W,
    DMM_MODE_CAPACITANCE,
    DMM_MODE_FREQUENCY,
    DMM_MODE_PERIOD,
    DMM_MODE_DIODE,
    DMM_MODE_CONTINUITY,
    DMM_MODE_TEMPERATURE,

    DMM_MODE_COUNT
} dmm_mode_t;

/* Polling / integration rate. Mirrors OWON XDM's RATE S/M/F and Keysight's
 * NPLC presets (FAST≈0.02, MED≈1, SLOW≈10). Drivers may map them to
 * whatever the instrument supports. */
typedef enum {
    DMM_RATE_SLOW   = 0,
    DMM_RATE_MEDIUM = 1,
    DMM_RATE_FAST   = 2,
} dmm_rate_t;

/* Snapshot of the meter's latest reading. */
typedef struct {
    dmm_mode_t mode;          /* what's being measured right now */
    float      value;         /* primary reading in base SI units
                                 (V, A, Ω, F, Hz, s, °C, V for diode) */
    bool       overload;      /* OL / overrange */
    bool       valid;         /* false until first sample arrives */
    float      range;         /* current range (0 = auto) */
    dmm_rate_t rate;
    uint64_t   timestamp_ms;  /* monotonic ms when sampled */
} dmm_reading_t;

/* Compile-time capability description filled in by the factory. */
typedef struct {
    const char *model_name;
    bool        supports_mode[DMM_MODE_COUNT];
    bool        supports_rate_control;
    bool        supports_range_control;
    int         display_digits;   /* nominal: 4 (4½), 5 (5½), 6 (6½) … */
} dmm_caps_t;

/* DMM instance. */
typedef struct dmm_driver dmm_driver_t;
struct dmm_driver {
    dmm_caps_t caps;
    void      *state;

    void (*close)        (dmm_driver_t *self);
    bool (*is_connected) (dmm_driver_t *self);
    void (*read)         (dmm_driver_t *self, dmm_reading_t *out);

    /* Setters - return false on transport error. NULL ⇒ not supported. */
    bool (*set_mode)     (dmm_driver_t *self, dmm_mode_t mode);
    bool (*set_range)    (dmm_driver_t *self, float range);   /* 0 = auto */
    bool (*set_rate)     (dmm_driver_t *self, dmm_rate_t rate);

    void (*get_stats)    (dmm_driver_t *self, uint32_t *rx_count, uint32_t *err_count);
};

/* Factory describing a DMM model. */
typedef struct {
    const char *id;            /* stable id, used on CLI: "owon-xdm", "dmm-demo" */
    const char *display_name;  /* shown in launcher */
    const char *description;   /* one-line tooltip */
    int         default_baud;

    dmm_driver_t *(*open)(const char *device, int baud);
} dmm_driver_factory_t;

/* ---- helpers ---- */

/* Human-readable short label for a mode (e.g. "DCV", "ACV", "DCI", "OHM-2W"). */
const char *dmm_mode_label(dmm_mode_t m);

/* Engineering unit suffix for a mode ("V", "A", "Ω", "F", "Hz", "s", "°C"). */
const char *dmm_mode_unit(dmm_mode_t m);

#endif
