/**
 * Synthetic "demo" driver. Always "connected", returns sinusoidally varying
 * output values around its setpoints so views look alive without hardware.
 *
 * Open with any non-NULL device string (it is ignored), e.g.:
 *   psu_app --driver=demo --port=- --view=toolbar-single
 */

#include "demo.h"

#include "platform/platform.h"

#include <math.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>

#define V_MAX 36.0f
#define I_MAX  6.0f

typedef struct {
    pthread_mutex_t lock;

    /* Setpoints (writable from UI thread). */
    float set_v[2];
    float set_a[2];
    bool  out_on[2];
    bool  tracking;

    /* Animation phase. */
    uint64_t t0_ms;
} demo_state_t;

#define now_ms() pl_now_ms()

static demo_state_t *st_of(psu_driver_t *d) { return (demo_state_t *)d->state; }

static void demo_close(psu_driver_t *self) {
    if (!self) return;
    demo_state_t *s = st_of(self);
    if (s) {
        pthread_mutex_destroy(&s->lock);
        free(s);
    }
    free(self);
}

static bool demo_is_connected(psu_driver_t *self) {
    (void)self;
    return true;
}

static void demo_get_channel(psu_driver_t *self, int ch, psu_channel_state_t *out) {
    if (ch < 1 || ch > 2) { memset(out, 0, sizeof(*out)); return; }
    demo_state_t *s = st_of(self);
    int idx = ch - 1;

    pthread_mutex_lock(&s->lock);
    float sv = s->set_v[idx];
    float sa = s->set_a[idx];
    bool  on = s->out_on[idx];
    uint64_t t0 = s->t0_ms;
    pthread_mutex_unlock(&s->lock);

    float t = (now_ms() - t0) / 1000.0f;
    float phase = t * 1.2f + idx * 0.8f;

    memset(out, 0, sizeof(*out));
    out->set_v = sv;
    out->set_a = sa;
    if (on) {
        out->out_v = sv - (0.20f + 0.10f * sinf(phase));
        out->out_a = sa - (0.030f + 0.015f * sinf(phase * 0.7f));
        if (out->out_v < 0) out->out_v = 0;
        if (out->out_a < 0) out->out_a = 0;
        out->out_p = out->out_v * out->out_a;
    }
    out->in_v        = 24.0f;
    out->temp_c      = 32.0f + 4.0f * sinf(phase * 0.3f);
    out->runtime_s   = (uint32_t)t;
    out->out_on      = on;
    out->cv_mode     = (out->out_a < sa - 0.020f);
    out->valid       = true;
    out->timestamp_ms = now_ms();
}

static bool demo_set_voltage(psu_driver_t *self, int ch, float v) {
    if (ch < 1 || ch > 2 || v < 0 || v > V_MAX) return false;
    demo_state_t *s = st_of(self);
    pthread_mutex_lock(&s->lock);
    s->set_v[ch - 1] = v;
    if (s->tracking && ch == 1) s->set_v[1] = v;
    pthread_mutex_unlock(&s->lock);
    return true;
}

static bool demo_set_current(psu_driver_t *self, int ch, float a) {
    if (ch < 1 || ch > 2 || a < 0 || a > I_MAX) return false;
    demo_state_t *s = st_of(self);
    pthread_mutex_lock(&s->lock);
    s->set_a[ch - 1] = a;
    if (s->tracking && ch == 1) s->set_a[1] = a;
    pthread_mutex_unlock(&s->lock);
    return true;
}

static bool demo_set_output(psu_driver_t *self, int ch, bool on) {
    if (ch < 1 || ch > 2) return false;
    demo_state_t *s = st_of(self);
    pthread_mutex_lock(&s->lock);
    s->out_on[ch - 1] = on;
    pthread_mutex_unlock(&s->lock);
    return true;
}

static bool demo_set_tracking(psu_driver_t *self, bool on) {
    demo_state_t *s = st_of(self);
    pthread_mutex_lock(&s->lock);
    s->tracking = on;
    if (on) {
        s->set_v[1] = s->set_v[0];
        s->set_a[1] = s->set_a[0];
    }
    pthread_mutex_unlock(&s->lock);
    return true;
}

static psu_driver_t *demo_open(const char *device, int baud) {
    (void)device;
    (void)baud;

    demo_state_t *s = calloc(1, sizeof(*s));
    if (!s) return NULL;
    pthread_mutex_init(&s->lock, NULL);
    s->set_v[0] = s->set_v[1] = 12.00f;
    s->set_a[0] = s->set_a[1] = 1.500f;
    s->t0_ms = now_ms();

    psu_driver_t *d = calloc(1, sizeof(*d));
    if (!d) {
        pthread_mutex_destroy(&s->lock);
        free(s);
        return NULL;
    }
    d->state = s;
    d->caps = (psu_caps_t){
        .model_name             = "Demo PSU (synthetic)",
        .n_channels             = 2,
        .v_max                  = V_MAX,
        .i_max                  = I_MAX,
        .supports_tracking      = true,
        .supports_mppt          = false,
        .supports_ovp           = false,
        .supports_temperature   = true,
        .supports_input_voltage = true,
        .supports_runtime       = true,
        .supports_energy        = false,
    };
    d->close        = demo_close;
    d->is_connected = demo_is_connected;
    d->get_channel  = demo_get_channel;
    d->set_voltage  = demo_set_voltage;
    d->set_current  = demo_set_current;
    d->set_output   = demo_set_output;
    d->set_tracking = demo_set_tracking;
    d->get_stats    = NULL;
    return d;
}

const psu_driver_factory_t demo_factory = {
    .id              = "demo",
    .display_name    = "Demo (synthetic)",
    .description     = "Synthetic 2-channel PSU - animated values, no hardware required",
    .default_baud    = 0,
    .n_channels_hint = 2,
    .open            = demo_open,
};
