/**
 * Synthetic DMM. Always "connected", returns a sinusoidally-varying value
 * around a per-mode baseline so DMM views look alive without hardware.
 */

#include "dmm_demo.h"

#include "platform/platform.h"

#include <math.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    pthread_mutex_t lock;
    dmm_mode_t      mode;
    float           range;     /* 0 = auto */
    dmm_rate_t      rate;
    uint64_t        t0_ms;
} demo_state_t;

#define now_ms() pl_now_ms()

static demo_state_t *st_of(dmm_driver_t *d) { return (demo_state_t *)d->state; }

static void v_close(dmm_driver_t *self) {
    if (!self) return;
    demo_state_t *s = st_of(self);
    if (s) {
        pthread_mutex_destroy(&s->lock);
        free(s);
    }
    free(self);
}

static bool v_is_connected(dmm_driver_t *self) { (void)self; return true; }

/* Return a plausible baseline + small wobble for the given mode. */
static float synth_value(dmm_mode_t mode, float t) {
    float wobble = sinf(t * 1.3f);
    switch (mode) {
        case DMM_MODE_DC_VOLTS:    return  4.823456f + 0.001234f * wobble;
        case DMM_MODE_AC_VOLTS:    return  3.456789f + 0.002500f * wobble;
        case DMM_MODE_DC_AMPS:     return  0.123456f + 0.000123f * wobble;
        case DMM_MODE_AC_AMPS:     return  0.045678f + 0.000234f * wobble;
        case DMM_MODE_OHMS_2W:     return 1234.567f  + 1.234f    * wobble;
        case DMM_MODE_OHMS_4W:     return 9876.543f  + 0.987f    * wobble;
        case DMM_MODE_CAPACITANCE: return 0.0001047f + 0.0000005f * wobble;     /* ≈100 nF */
        case DMM_MODE_FREQUENCY:   return  50.00000f + 0.00010f  * wobble;
        case DMM_MODE_PERIOD:      return  0.020000f + 0.000001f * wobble;
        case DMM_MODE_DIODE:       return  0.598234f + 0.000100f * wobble;
        case DMM_MODE_CONTINUITY:  return  0.5f      + 0.05f     * wobble;
        case DMM_MODE_TEMPERATURE: return 23.45f     + 0.12f     * wobble;
        default:                   return 0.0f;
    }
}

static void v_read(dmm_driver_t *self, dmm_reading_t *out) {
    demo_state_t *s = st_of(self);
    pthread_mutex_lock(&s->lock);
    dmm_mode_t mode = s->mode;
    float     range = s->range;
    dmm_rate_t rate = s->rate;
    uint64_t  t0    = s->t0_ms;
    pthread_mutex_unlock(&s->lock);

    float t = (now_ms() - t0) / 1000.0f;
    memset(out, 0, sizeof(*out));
    out->mode         = mode;
    out->value        = synth_value(mode, t);
    out->overload     = false;
    out->valid        = true;
    out->range        = range;
    out->rate         = rate;
    out->timestamp_ms = now_ms();
}

static bool v_set_mode(dmm_driver_t *self, dmm_mode_t mode) {
    if (mode < 0 || mode >= DMM_MODE_COUNT) return false;
    demo_state_t *s = st_of(self);
    pthread_mutex_lock(&s->lock); s->mode = mode; pthread_mutex_unlock(&s->lock);
    return true;
}

static bool v_set_range(dmm_driver_t *self, float range) {
    if (range < 0) return false;
    demo_state_t *s = st_of(self);
    pthread_mutex_lock(&s->lock); s->range = range; pthread_mutex_unlock(&s->lock);
    return true;
}

static bool v_set_rate(dmm_driver_t *self, dmm_rate_t rate) {
    demo_state_t *s = st_of(self);
    pthread_mutex_lock(&s->lock); s->rate = rate; pthread_mutex_unlock(&s->lock);
    return true;
}

static dmm_driver_t *demo_open(const char *device, int baud) {
    (void)device;
    (void)baud;

    demo_state_t *s = calloc(1, sizeof(*s));
    if (!s) return NULL;
    pthread_mutex_init(&s->lock, NULL);
    s->mode   = DMM_MODE_DC_VOLTS;
    s->range  = 0;             /* auto */
    s->rate   = DMM_RATE_MEDIUM;
    s->t0_ms  = now_ms();

    dmm_driver_t *d = calloc(1, sizeof(*d));
    if (!d) { pthread_mutex_destroy(&s->lock); free(s); return NULL; }

    d->state = s;
    d->caps  = (dmm_caps_t){ .model_name = "Demo DMM (synthetic)",
                             .supports_rate_control  = true,
                             .supports_range_control = true,
                             .display_digits         = 6 };
    /* All modes available in the demo. */
    for (int i = 0; i < DMM_MODE_COUNT; i++) d->caps.supports_mode[i] = true;

    d->close        = v_close;
    d->is_connected = v_is_connected;
    d->read         = v_read;
    d->set_mode     = v_set_mode;
    d->set_range    = v_set_range;
    d->set_rate     = v_set_rate;
    d->get_stats    = NULL;
    return d;
}

const dmm_driver_factory_t dmm_demo_factory = {
    .id           = "dmm-demo",
    .display_name = "Demo DMM (synthetic)",
    .description  = "Synthetic 6½-digit DMM - animated readings, no hardware required",
    .default_baud = 0,
    .open         = demo_open,
};
