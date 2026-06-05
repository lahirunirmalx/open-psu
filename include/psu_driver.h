/**
 * psu_driver.h - UI-facing PSU driver interface.
 *
 * Views (full / single / toolbar) talk only to this interface. Concrete
 * drivers (Modbus bridge, SCPI/Siglent, …) live under drivers/ and implement
 * this vtable. All values are in SI units (volts, amps, watts, °C, seconds)
 * so views never see wire-level scaling.
 *
 * Thread-safety: drivers must make get_channel(), is_connected(), and the
 * setter calls safe to call from the UI thread while internal reader threads
 * are running. Snapshots returned by get_channel() are independent copies.
 *
 * Optional features: any field marked "optional" may be NAN (floats) or 0
 * (integers) if the driver / hardware doesn't expose it. Views should check
 * the matching psu_caps_t flag before showing such fields.
 */

#ifndef PSU_DRIVER_H
#define PSU_DRIVER_H

#include <stdbool.h>
#include <stdint.h>

/* One snapshot of a single channel's live state. */
typedef struct {
    /* Setpoints */
    float    set_v;          /* Voltage setpoint, V */
    float    set_a;          /* Current limit, A */

    /* Live output */
    float    out_v;          /* Output voltage, V */
    float    out_a;          /* Output current, A */
    float    out_p;          /* Output power, W */
    float    out_energy_wh;  /* optional - energy delivered, Wh */

    /* Environment */
    float    in_v;           /* optional - input/rail voltage, V */
    float    temp_c;         /* optional - temperature, °C */

    /* Timers */
    uint32_t runtime_s;      /* optional - output-on runtime, s */
    float    capacity_ah;    /* optional - accumulated Ah */

    /* Protection thresholds (optional) */
    float    ovp;            /* over-voltage protection, V */
    float    ocp;            /* over-current protection, A */
    float    opp;            /* over-power protection, W */

    /* Mode / state flags */
    bool     cv_mode;        /* true = CV regulating, false = CC */
    bool     out_on;         /* output enabled */
    bool     mppt;           /* MPPT mode active (optional) */

    /* Meta */
    bool     valid;          /* true once at least one valid sample has arrived */
    uint64_t timestamp_ms;   /* monotonic ms when this snapshot was captured */
} psu_channel_state_t;

/* What the driver/hardware combo supports. Filled by the factory. */
typedef struct {
    const char *model_name;        /* e.g. "ESP32 Modbus Bridge", "Siglent SPD3303" */
    int         n_channels;        /* 1 or 2 */
    float       v_max;             /* per-channel hardware limit, V */
    float       i_max;             /* per-channel hardware limit, A */
    bool        supports_tracking; /* set_tracking() is non-NULL */
    bool        supports_mppt;
    bool        supports_ovp;      /* ovp/ocp/opp fields populated */
    bool        supports_temperature;
    bool        supports_input_voltage;
    bool        supports_runtime;
    bool        supports_energy;
} psu_caps_t;

/* Driver instance - opened by a factory, used by views, closed by close(). */
typedef struct psu_driver psu_driver_t;
struct psu_driver {
    /* Filled by factory before returning. */
    psu_caps_t caps;
    void      *state;      /* driver-private */

    /* Lifecycle. */
    void  (*close)         (psu_driver_t *self);

    /* Polling / status. */
    bool  (*is_connected)  (psu_driver_t *self);
    void  (*get_channel)   (psu_driver_t *self, int ch, psu_channel_state_t *out);

    /* Setpoints / control. Return false on transport error. */
    bool  (*set_voltage)   (psu_driver_t *self, int ch, float volts);
    bool  (*set_current)   (psu_driver_t *self, int ch, float amps);
    bool  (*set_output)    (psu_driver_t *self, int ch, bool enable);

    /* Optional: NULL if caps.supports_tracking is false. */
    bool  (*set_tracking)  (psu_driver_t *self, bool enable);

    /* Optional: link-level stats. NULL if not meaningful. */
    void  (*get_stats)     (psu_driver_t *self, uint32_t *rx_count, uint32_t *err_count);
};

/* A factory describes a driver type and knows how to open instances of it. */
typedef struct {
    const char *id;              /* stable id, used on CLI: "modbus-bridge", "siglent-spd" */
    const char *display_name;    /* shown in launcher */
    const char *description;     /* one-line tooltip */
    int         default_baud;
    int         n_channels_hint; /* expected channel count, used by the launcher to
                                  * grey out 2-ch views before open(). The opened
                                  * driver reports the authoritative count in caps. */

    /*
     * Open a new driver instance. Returns NULL on failure (port not found,
     * handshake timeout, etc.). On success the returned driver is fully
     * usable: caps filled, vtable wired, reader thread (if any) running.
     */
    psu_driver_t *(*open)(const char *device, int baud);
} psu_driver_factory_t;

#endif /* PSU_DRIVER_H */
