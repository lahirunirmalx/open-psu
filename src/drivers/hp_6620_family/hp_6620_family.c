/**
 * HP / Agilent 6620-series multi-output system DC sources — native HP DCL
 * (predates SCPI on this family). One implementation parameterised by a
 * per-model profile that names the channel count and per-channel V/I
 * limits.
 *
 * Wire commands:
 *
 *     ID?                                   identification
 *     VSET <ch>,<v>                         set voltage
 *     ISET <ch>,<a>                         set current limit
 *     OUT  <ch>,<0|1>                       output enable
 *     VOUT? <ch>                            measured voltage
 *     IOUT? <ch>                            measured current
 *     VSET? <ch>                            setpoint voltage
 *     ISET? <ch>                            setpoint current
 *     STS?  <ch>                            status bitmap (we read bit 1
 *                                            = constant-current, bit 0
 *                                            = constant-voltage, bit 7 =
 *                                            output enabled — model-
 *                                            specific; refer to manual)
 *
 * GPIB-only — reach via a Prologix controller:
 *     --port=prologix:/dev/ttyUSB0:<gpib-addr>
 */

#include "hp_6620_family.h"

#include "platform/platform.h"
#include "transport/scpi.h"

#include <math.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define POLL_MS          250
#define IDN_TIMEOUT_MS   800
#define QUERY_TIMEOUT_MS 1200
#define SETUP_TIMEOUT_MS 800

#define MAX_CHANNELS 4

typedef struct {
    const char *model_name;
    int   n_channels;
    float v_max[MAX_CHANNELS];
    float i_max[MAX_CHANNELS];
} hp6620_profile_t;

/* HP 6622A — dual output, 2 × 50V/2A. */
static const hp6620_profile_t k_prof_6622a = {
    .model_name = "HP 6622A System DC Source (2-output)",
    .n_channels = 2,
    .v_max = {50.0f, 50.0f},
    .i_max = { 2.0f,  2.0f},
};

/* HP 6623A — triple output: two 20V/2A + one 50V/0.8A
 * (Agilent's standard config; some variants differ). */
static const hp6620_profile_t k_prof_6623a = {
    .model_name = "HP 6623A System DC Source (3-output)",
    .n_channels = 3,
    .v_max = {20.0f, 20.0f, 50.0f},
    .i_max = { 2.0f,  2.0f,  0.8f},
};

/* HP 6624A — quad output: two 7V/5A + two 20V/2A. */
static const hp6620_profile_t k_prof_6624a = {
    .model_name = "HP 6624A System DC Source (4-output)",
    .n_channels = 4,
    .v_max = { 7.0f,  20.0f,  7.0f, 20.0f},
    .i_max = { 5.0f,   2.0f,  5.0f,  2.0f},
};

/* HP 6625A — dual output, higher-power per channel: 2 × 50V/1A. */
static const hp6620_profile_t k_prof_6625a = {
    .model_name = "HP 6625A System DC Source (2-output, high-power)",
    .n_channels = 2,
    .v_max = {50.0f, 50.0f},
    .i_max = { 1.0f,  1.0f},
};

/* HP 6627A — quad output: 4 × 50V/2A. */
static const hp6620_profile_t k_prof_6627a = {
    .model_name = "HP 6627A System DC Source (4-output)",
    .n_channels = 4,
    .v_max = {50.0f, 50.0f, 50.0f, 50.0f},
    .i_max = { 2.0f,  2.0f,  2.0f,  2.0f},
};

typedef struct {
    const hp6620_profile_t *prof;
    scpi_t *scpi;

    pthread_t      reader;
    volatile bool  running;
    volatile bool  connected;

    pthread_mutex_t state_lock;
    psu_channel_state_t state[MAX_CHANNELS];

    /* Local cache for setpoints / output state — STS? bits are
     * model-specific; trusting our own writes is more portable. */
    float set_v[MAX_CHANNELS];
    float set_a[MAX_CHANNELS];
    bool  out_on[MAX_CHANNELS];

    volatile uint32_t rx_count;
    volatile uint32_t err_count;
} hp66_state_t;

static hp66_state_t *st_of(psu_driver_t *d) { return (hp66_state_t *)d->state; }

static void send_setup(hp66_state_t *s, const char *cmd) {
    char buf[64];
    if (!scpi_query(s->scpi, cmd, buf, sizeof(buf), SETUP_TIMEOUT_MS)) {
        /* Setup commands don't reply; auto-read timeout is expected. */
    }
}

static bool query_float(hp66_state_t *s, const char *fmt, int ch, float *out) {
    char cmd[64], resp[64];
    snprintf(cmd, sizeof(cmd), fmt, ch);
    if (!scpi_query(s->scpi, cmd, resp, sizeof(resp), QUERY_TIMEOUT_MS)) {
        s->err_count++;
        return false;
    }
    char *end = NULL;
    float v = strtof(resp, &end);
    if (end == resp) { s->err_count++; return false; }
    s->rx_count++;
    *out = v;
    return true;
}

/* ----- reader thread ---------------------------------------------------- */

static void *reader_main(void *arg) {
    psu_driver_t *d = (psu_driver_t *)arg;
    hp66_state_t *s = st_of(d);

    char idn[160] = {0};
    if (scpi_query(s->scpi, "ID?", idn, sizeof(idn), IDN_TIMEOUT_MS)) {
        fprintf(stderr, "hp-6620: %s says: %s", s->prof->model_name, idn);
        if (strchr(idn, '\n') == NULL) fputc('\n', stderr);
        s->connected = true;
        s->rx_count++;
    } else {
        fprintf(stderr, "hp-6620: ID? timed out — continuing anyway\n");
        s->err_count++;
    }

    /* No global init commands needed — DCL leaves the instrument in
     * whatever state the user last set. */

    while (s->running) {
        for (int ch = 1; ch <= s->prof->n_channels && s->running; ch++) {
            float v = 0, a = 0;
            bool ok_v = query_float(s, "VOUT? %d", ch, &v);
            bool ok_a = query_float(s, "IOUT? %d", ch, &a);

            pthread_mutex_lock(&s->state_lock);
            psu_channel_state_t *cs = &s->state[ch - 1];
            if (ok_v) cs->out_v = v;
            if (ok_a) cs->out_a = a;
            cs->out_p   = (ok_v && ok_a) ? (v * a) : 0;
            cs->set_v   = s->set_v[ch - 1];
            cs->set_a   = s->set_a[ch - 1];
            cs->out_on  = s->out_on[ch - 1];
            /* CV/CC heuristic: in CC the output current sits at the
             * setpoint while voltage falls below the setting. */
            cs->cv_mode = !(cs->out_on
                          && cs->out_v < cs->set_v - 0.05f
                          && cs->out_a >= cs->set_a - 0.01f);
            cs->valid   = ok_v && ok_a;
            if (cs->valid) {
                cs->timestamp_ms = pl_now_ms();
                s->connected = true;
            }
            pthread_mutex_unlock(&s->state_lock);
        }
        pl_sleep_ms(POLL_MS);
    }
    return NULL;
}

/* ----- vtable ----------------------------------------------------------- */

static void v_close(psu_driver_t *self) {
    if (!self) return;
    hp66_state_t *s = st_of(self);
    if (s) {
        s->running = false;
        pthread_join(s->reader, NULL);
        if (s->scpi) scpi_close(s->scpi);
        pthread_mutex_destroy(&s->state_lock);
        free(s);
    }
    free(self);
}

static bool v_is_connected(psu_driver_t *self) { return st_of(self)->connected; }

static void v_get_channel(psu_driver_t *self, int ch, psu_channel_state_t *out) {
    hp66_state_t *s = st_of(self);
    if (ch < 1 || ch > s->prof->n_channels) { memset(out, 0, sizeof(*out)); return; }
    pthread_mutex_lock(&s->state_lock);
    *out = s->state[ch - 1];
    pthread_mutex_unlock(&s->state_lock);
}

static bool v_set_voltage(psu_driver_t *self, int ch, float v) {
    hp66_state_t *s = st_of(self);
    if (ch < 1 || ch > s->prof->n_channels) return false;
    if (v < 0 || v > s->prof->v_max[ch - 1]) return false;
    pthread_mutex_lock(&s->state_lock); s->set_v[ch - 1] = v; pthread_mutex_unlock(&s->state_lock);
    char cmd[64];
    snprintf(cmd, sizeof(cmd), "VSET %d,%.4f", ch, (double)v);
    send_setup(s, cmd);
    return true;
}

static bool v_set_current(psu_driver_t *self, int ch, float a) {
    hp66_state_t *s = st_of(self);
    if (ch < 1 || ch > s->prof->n_channels) return false;
    if (a < 0 || a > s->prof->i_max[ch - 1]) return false;
    pthread_mutex_lock(&s->state_lock); s->set_a[ch - 1] = a; pthread_mutex_unlock(&s->state_lock);
    char cmd[64];
    snprintf(cmd, sizeof(cmd), "ISET %d,%.4f", ch, (double)a);
    send_setup(s, cmd);
    return true;
}

static bool v_set_output(psu_driver_t *self, int ch, bool on) {
    hp66_state_t *s = st_of(self);
    if (ch < 1 || ch > s->prof->n_channels) return false;
    pthread_mutex_lock(&s->state_lock); s->out_on[ch - 1] = on; pthread_mutex_unlock(&s->state_lock);
    char cmd[32];
    snprintf(cmd, sizeof(cmd), "OUT %d,%d", ch, on ? 1 : 0);
    send_setup(s, cmd);
    return true;
}

static void v_get_stats(psu_driver_t *self, uint32_t *rx, uint32_t *err) {
    hp66_state_t *s = st_of(self);
    if (rx)  *rx  = s->rx_count;
    if (err) *err = s->err_count;
}

/* ----- factory ---------------------------------------------------------- */

static psu_driver_t *hp_dcl_psu_open(const hp6620_profile_t *prof,
                                     const char *port_spec, int default_baud) {
    if (!prof || !port_spec) return NULL;

    scpi_t *scpi = scpi_open(port_spec, default_baud);
    if (!scpi) {
        fprintf(stderr, "hp-6620: failed to open transport for '%s'\n", port_spec);
        return NULL;
    }

    hp66_state_t *s = calloc(1, sizeof(*s));
    psu_driver_t *d = calloc(1, sizeof(*d));
    if (!s || !d) { free(s); free(d); scpi_close(scpi); return NULL; }

    s->prof = prof;
    s->scpi = scpi;
    s->running = true;
    pthread_mutex_init(&s->state_lock, NULL);

    /* Reasonable defaults for the local cache. */
    for (int i = 0; i < prof->n_channels; i++) {
        s->set_v[i]  = 0.0f;
        s->set_a[i]  = prof->i_max[i] * 0.1f;
        s->out_on[i] = false;
        s->state[i].set_v = s->set_v[i];
        s->state[i].set_a = s->set_a[i];
    }

    float v_lim = 0, i_lim = 0;
    for (int i = 0; i < prof->n_channels; i++) {
        if (prof->v_max[i] > v_lim) v_lim = prof->v_max[i];
        if (prof->i_max[i] > i_lim) i_lim = prof->i_max[i];
    }

    d->state = s;
    d->caps = (psu_caps_t){
        .model_name             = prof->model_name,
        .n_channels             = prof->n_channels,
        .v_max                  = v_lim,
        .i_max                  = i_lim,
        .supports_tracking      = false,
        .supports_mppt          = false,
        .supports_ovp           = false,
        .supports_temperature   = false,
        .supports_input_voltage = false,
        .supports_runtime       = false,
        .supports_energy        = false,
    };
    d->close        = v_close;
    d->is_connected = v_is_connected;
    d->get_channel  = v_get_channel;
    d->set_voltage  = v_set_voltage;
    d->set_current  = v_set_current;
    d->set_output   = v_set_output;
    d->set_tracking = NULL;
    d->get_stats    = v_get_stats;

    if (pthread_create(&s->reader, NULL, reader_main, d) != 0) {
        v_close(d);
        return NULL;
    }
    return d;
}

#define DEF(name, ID, DISPLAY, DESC, HINT, PROFILE)                          \
    static psu_driver_t *open_##name(const char *p, int b) {                 \
        return hp_dcl_psu_open(&PROFILE, p, b);                              \
    }                                                                         \
    const psu_driver_factory_t name##_factory = {                            \
        .id              = ID,                                               \
        .display_name    = DISPLAY,                                          \
        .description     = DESC,                                             \
        .default_baud    = 115200,                                           \
        .n_channels_hint = HINT,                                             \
        .open            = open_##name,                                      \
    }

DEF(hp_6622a, "hp-6622a",
    "HP 6622A — 2-output system DC source (DCL)",
    "Native HP-IB, not SCPI. 2 × 50V/2A. Use prologix:<dev>:<gpib-addr>.",
    2, k_prof_6622a);

DEF(hp_6623a, "hp-6623a",
    "HP 6623A — 3-output system DC source (DCL)",
    "Native HP-IB, not SCPI. 20V/2A x2 + 50V/0.8A. Use prologix:<dev>:<gpib-addr>.",
    3, k_prof_6623a);

DEF(hp_6624a, "hp-6624a",
    "HP 6624A — 4-output system DC source (DCL)",
    "Native HP-IB, not SCPI. 7V/5A x2 + 20V/2A x2. Use prologix:<dev>:<gpib-addr>.",
    4, k_prof_6624a);

DEF(hp_6625a, "hp-6625a",
    "HP 6625A — 2-output high-power system DC source (DCL)",
    "Native HP-IB, not SCPI. 2 × 50V/1A. Use prologix:<dev>:<gpib-addr>.",
    2, k_prof_6625a);

DEF(hp_6627a, "hp-6627a",
    "HP 6627A — 4-output system DC source (DCL)",
    "Native HP-IB, not SCPI. 4 × 50V/2A. Use prologix:<dev>:<gpib-addr>.",
    4, k_prof_6627a);
