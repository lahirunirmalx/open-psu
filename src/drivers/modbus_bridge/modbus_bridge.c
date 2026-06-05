/**
 * Modbus-bridge driver - wraps the existing psu_protocol.c transport so that
 * the rest of the app can talk to it through the generic psu_driver_t vtable.
 *
 * The bridge firmware emits status as scaled integers (V*100, A*1000, …); we
 * convert to floats here so views never see wire encoding.
 */

#include "modbus_bridge.h"

#include "psu_protocol.h"

#include <stdlib.h>
#include <string.h>

/* Wire scaling used by the ESP32 bridge firmware. */
#define V_SCALE       100.0f   /* register value = volts * 100 */
#define A_SCALE       1000.0f  /* register value = amps  * 1000 */
#define P_SCALE       100.0f   /* register value = watts * 100 */
#define TEMP_SCALE    10.0f    /* register value = °C    * 10 */
#define CAP_SCALE     1000.0f  /* register value = Ah    * 1000 */

/* Module-wide assumed hardware limits - Riden RD60xx-class, the bridge's
 * target. Could be made driver-side configurable later. */
#define V_MAX         60.0f
#define I_MAX         20.0f

typedef struct {
    psu_context_t psu;
} mb_state_t;

static mb_state_t *as_state(psu_driver_t *d) { return (mb_state_t *)d->state; }

static void mb_close(psu_driver_t *self) {
    if (!self) return;
    mb_state_t *s = as_state(self);
    if (s) {
        psu_shutdown(&s->psu);
        free(s);
    }
    free(self);
}

static bool mb_is_connected(psu_driver_t *self) {
    return psu_is_connected(&as_state(self)->psu);
}

static void mb_get_channel(psu_driver_t *self, int ch, psu_channel_state_t *out) {
    psu_status_t raw;
    psu_get_status(&as_state(self)->psu, ch, &raw);

    memset(out, 0, sizeof(*out));
    out->set_v         = raw.set_v   / V_SCALE;
    out->set_a         = raw.set_a   / A_SCALE;
    out->out_v         = raw.out_v   / V_SCALE;
    out->out_a         = raw.out_a   / A_SCALE;
    out->out_p         = raw.out_p   / P_SCALE;
    out->out_energy_wh = (float)raw.out_e;
    out->in_v          = raw.in_v    / V_SCALE;
    out->temp_c        = raw.temp    / TEMP_SCALE;
    out->runtime_s     = raw.runtime;
    out->capacity_ah   = raw.capacity / CAP_SCALE;
    out->ovp           = raw.ovp / V_SCALE;
    out->ocp           = raw.ocp / A_SCALE;
    out->opp           = raw.opp / P_SCALE;
    out->cv_mode       = (raw.cvcc == 0);
    out->out_on        = (raw.out_on != 0);
    out->mppt          = (raw.mppt != 0);
    out->valid         = raw.valid;
    out->timestamp_ms  = raw.timestamp_ms;
}

static bool mb_set_voltage(psu_driver_t *self, int ch, float v) {
    return psu_set_voltage(&as_state(self)->psu, ch, v);
}

static bool mb_set_current(psu_driver_t *self, int ch, float a) {
    return psu_set_current(&as_state(self)->psu, ch, a);
}

static bool mb_set_output(psu_driver_t *self, int ch, bool on) {
    return psu_set_output(&as_state(self)->psu, ch, on);
}

static bool mb_set_tracking(psu_driver_t *self, bool on) {
    /* Underlying protocol's LINK command copies Ch1 to Ch2 - there is no
     * "untrack" on the wire; passing false is a no-op from the firmware's
     * perspective. We surface it as a one-shot apply when enabled. */
    if (!on) return true;
    return psu_link(&as_state(self)->psu);
}

static void mb_get_stats(psu_driver_t *self, uint32_t *rx, uint32_t *err) {
    psu_get_stats(&as_state(self)->psu, rx, err);
}

static psu_driver_t *mb_open(const char *device, int baud) {
    if (!device) return NULL;

    mb_state_t *s = calloc(1, sizeof(*s));
    if (!s) return NULL;

    if (!psu_init(&s->psu, device, baud)) {
        free(s);
        return NULL;
    }

    psu_driver_t *d = calloc(1, sizeof(*d));
    if (!d) {
        psu_shutdown(&s->psu);
        free(s);
        return NULL;
    }

    d->state = s;
    d->caps = (psu_caps_t){
        .model_name             = "Riden RD60xx via ESP32 Modbus bridge",
        .n_channels             = 2,
        .v_max                  = V_MAX,
        .i_max                  = I_MAX,
        .supports_tracking      = true,
        .supports_mppt          = true,
        .supports_ovp           = true,
        .supports_temperature   = true,
        .supports_input_voltage = true,
        .supports_runtime       = true,
        .supports_energy        = true,
    };
    d->close        = mb_close;
    d->is_connected = mb_is_connected;
    d->get_channel  = mb_get_channel;
    d->set_voltage  = mb_set_voltage;
    d->set_current  = mb_set_current;
    d->set_output   = mb_set_output;
    d->set_tracking = mb_set_tracking;
    d->get_stats    = mb_get_stats;
    return d;
}

const psu_driver_factory_t modbus_bridge_factory = {
    .id              = "modbus-bridge",
    .display_name    = "ESP32 Modbus Bridge",
    .description     = "Riden / DPS-style PSU fronted by ESP32 firmware over USB serial",
    .default_baud    = 115200,
    .n_channels_hint = 2,
    .open            = mb_open,
};
