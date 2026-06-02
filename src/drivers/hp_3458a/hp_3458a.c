/**
 * HP 3458A — 8½-digit GPIB DMM.
 *
 * The 3458A's wire dialect is HP's older "DCL" command set, NOT SCPI.
 * Selecting a function and reading a value looks like:
 *
 *     DCV 10              configure for DC volts, 10V range (AUTO also OK)
 *     NPLC 10             10 power-line-cycles integration (~170 ms / read)
 *     TARM SGL            arm + trigger a single measurement
 *     <- "+1.234567890E+00\r\n"   instrument sends the measurement
 *
 *     FUNC?               returns the function code as an ASCII float:
 *                           1 = DCV   2 = ACV   3 = ACDCV
 *                           4 = OHM   5 = OHMF
 *                           6 = DCI   7 = ACI   8 = ACDCI
 *                           9 = FREQ  10 = PER
 *
 * The driver assumes the user wired the instrument up through a Prologix
 * GPIB-USB-HPIB controller (--port=prologix:/dev/ttyUSB0:<addr>). The
 * Prologix's existing `++auto 1` mode auto-reads after each command, so
 * setup commands incur a small wasted-read timeout (~500 ms) but produce
 * no buffer contamination. Measurement commands (TARM SGL) ride the
 * auto-read so they're snappy.
 */

#include "hp_3458a.h"

#include "platform/platform.h"
#include "transport/scpi.h"

#include <ctype.h>
#include <math.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define POLL_MS          200
#define IDN_TIMEOUT_MS   800
#define QUERY_TIMEOUT_MS 1500    /* 3458A at slow NPLC can take >1s per read */
#define SETUP_TIMEOUT_MS 800     /* Prologix auto-read timeout on no-op writes */
#define FUNC_POLL_EVERY  8

/* Compile-time profile so the same wire-protocol implementation serves
 * both the 8½-digit 3458A and the 6½-digit 3457A. */
typedef struct {
    const char *model_name;
    int         display_digits;
    /* NPLC values for SLOW / MEDIUM / FAST. The 3457A floors NPLC much
     * earlier than the 3458A so we keep the table per-model. */
    float       nplc_slow;
    float       nplc_medium;
    float       nplc_fast;
} hp_dcl_profile_t;

static const hp_dcl_profile_t k_prof_3458a = {
    .model_name      = "HP 3458A (8½-digit reference DMM)",
    .display_digits  = 8,
    .nplc_slow       = 100.0f,
    .nplc_medium     = 10.0f,
    .nplc_fast       = 1.0f,
};

static const hp_dcl_profile_t k_prof_3457a = {
    .model_name      = "HP 3457A (6½-digit, DCL)",
    .display_digits  = 6,
    .nplc_slow       = 10.0f,
    .nplc_medium     = 1.0f,
    .nplc_fast       = 0.1f,
};

typedef struct {
    const hp_dcl_profile_t *prof;
    scpi_t *scpi;

    pthread_t      reader;
    volatile bool  running;
    volatile bool  connected;

    pthread_mutex_t state_lock;
    dmm_reading_t   state;

    dmm_mode_t mode_cache;
    float      range_cache;
    dmm_rate_t rate_cache;

    volatile uint32_t rx_count;
    volatile uint32_t err_count;
} hp_state_t;

static hp_state_t *st_of(dmm_driver_t *d) { return (hp_state_t *)d->state; }

/* ----- HP-language helpers ---------------------------------------------- */

/* Return the bare command keyword for the given mode, or NULL if the mode
 * isn't supported on this instrument. */
static const char *hp_func_keyword(dmm_mode_t m) {
    switch (m) {
        case DMM_MODE_DC_VOLTS:  return "DCV";
        case DMM_MODE_AC_VOLTS:  return "ACV";
        case DMM_MODE_DC_AMPS:   return "DCI";
        case DMM_MODE_AC_AMPS:   return "ACI";
        case DMM_MODE_OHMS_2W:   return "OHM";
        case DMM_MODE_OHMS_4W:   return "OHMF";
        case DMM_MODE_FREQUENCY: return "FREQ";
        case DMM_MODE_PERIOD:    return "PER";
        default:                 return NULL;
    }
}

/* Map FUNC? integer code to DMM_MODE_*. */
static dmm_mode_t hp_mode_from_code(int code, dmm_mode_t fallback) {
    switch (code) {
        case 1:  return DMM_MODE_DC_VOLTS;
        case 2:  /* ACV */
        case 3:  return DMM_MODE_AC_VOLTS;   /* ACDCV → fold into ACV */
        case 4:  return DMM_MODE_OHMS_2W;
        case 5:  return DMM_MODE_OHMS_4W;
        case 6:  return DMM_MODE_DC_AMPS;
        case 7:  /* ACI */
        case 8:  return DMM_MODE_AC_AMPS;    /* ACDCI → fold into ACI */
        case 9:  return DMM_MODE_FREQUENCY;
        case 10: return DMM_MODE_PERIOD;
        default: return fallback;
    }
}

static float nplc_for_rate(const hp_dcl_profile_t *p, dmm_rate_t r) {
    switch (r) {
        case DMM_RATE_SLOW:   return p->nplc_slow;
        case DMM_RATE_FAST:   return p->nplc_fast;
        case DMM_RATE_MEDIUM:
        default:              return p->nplc_medium;
    }
}

/* Send a configuration command and ignore the (empty) auto-read response. */
static void hp_send_setup(hp_state_t *s, const char *cmd) {
    char buf[64];
    /* Use scpi_query so the Prologix's auto-read timeout drains before we
     * issue the next command — avoids stale data on the serial side. */
    if (!scpi_query(s->scpi, cmd, buf, sizeof(buf), SETUP_TIMEOUT_MS)) {
        /* Empty reply is expected for setup commands; not an error. */
    }
}

/* Send the function-selection command for `mode` with a numeric range
 * (range <= 0 → "AUTO"). */
static void hp_send_function(hp_state_t *s, dmm_mode_t mode, float range) {
    const char *fn = hp_func_keyword(mode);
    if (!fn) return;
    char cmd[64];
    if (range <= 0) snprintf(cmd, sizeof(cmd), "%s AUTO", fn);
    else            snprintf(cmd, sizeof(cmd), "%s %g", fn, (double)range);
    hp_send_setup(s, cmd);
}

static void hp_send_nplc(hp_state_t *s, float nplc) {
    char cmd[32];
    snprintf(cmd, sizeof(cmd), "NPLC %g", (double)nplc);
    hp_send_setup(s, cmd);
}

/* ----- reader thread ---------------------------------------------------- */

static void *reader_main(void *arg) {
    dmm_driver_t *d = (dmm_driver_t *)arg;
    hp_state_t   *s = st_of(d);

    char idn[160] = {0};
    if (scpi_query(s->scpi, "ID?", idn, sizeof(idn), IDN_TIMEOUT_MS)) {
        /* Native 3458A: ID? returns "HP3458A". Some firmware also accepts
         * *IDN? — try both for diagnostics. */
        fprintf(stderr, "hp-dcl: ID? -> %s", idn);
        if (strchr(idn, '\n') == NULL) fputc('\n', stderr);
        s->connected = true;
        s->rx_count++;
    } else if (scpi_query(s->scpi, "*IDN?", idn, sizeof(idn), IDN_TIMEOUT_MS)) {
        fprintf(stderr, "hp-dcl: *IDN? -> %s", idn);
        if (strchr(idn, '\n') == NULL) fputc('\n', stderr);
        s->connected = true;
        s->rx_count++;
    } else {
        fprintf(stderr, "hp-dcl: identification timed out — continuing anyway\n");
        s->err_count++;
    }

    /* One-time setup: ASCII output, EOI-terminate every reading, hold the
     * trigger so we explicitly TARM SGL for each measurement. */
    hp_send_setup(s, "OFORMAT ASCII");
    hp_send_setup(s, "END ALWAYS");
    hp_send_setup(s, "TARM HOLD");
    hp_send_setup(s, "TRIG AUTO");
    hp_send_setup(s, "NRDGS 1,AUTO");

    /* Apply the initial mode + rate. */
    hp_send_function(s, s->mode_cache, s->range_cache);
    hp_send_nplc(s, nplc_for_rate(s->prof, s->rate_cache));

    int func_counter = 0;
    while (s->running) {
        /* Track external front-panel mode changes via FUNC?. */
        if (func_counter == 0) {
            char fbuf[32];
            if (scpi_query(s->scpi, "FUNC?", fbuf, sizeof(fbuf), QUERY_TIMEOUT_MS)) {
                s->rx_count++;
                int code = (int)strtof(fbuf, NULL);
                dmm_mode_t m = hp_mode_from_code(code, s->mode_cache);
                pthread_mutex_lock(&s->state_lock);
                s->state.mode = m;
                s->mode_cache = m;
                pthread_mutex_unlock(&s->state_lock);
            } else {
                s->err_count++;
            }
        }
        func_counter = (func_counter + 1) % FUNC_POLL_EVERY;

        /* TARM SGL: arm + trigger; the instrument emits exactly one
         * reading; Prologix's auto-read forwards it. */
        char mbuf[64];
        if (scpi_query(s->scpi, "TARM SGL", mbuf, sizeof(mbuf), QUERY_TIMEOUT_MS)) {
            char *end = NULL;
            float v = strtof(mbuf, &end);
            if (end != mbuf) {
                bool ol = (fabsf(v) > 9.0e36f);    /* 3458A overload constant */
                pthread_mutex_lock(&s->state_lock);
                s->state.value        = v;
                s->state.overload     = ol;
                s->state.valid        = true;
                s->state.timestamp_ms = pl_now_ms();
                s->state.range        = s->range_cache;
                s->state.rate         = s->rate_cache;
                pthread_mutex_unlock(&s->state_lock);
                s->connected = true;
                s->rx_count++;
            } else {
                s->err_count++;
            }
        } else {
            s->err_count++;
        }

        pl_sleep_ms(POLL_MS);
    }
    return NULL;
}

/* ----- vtable ----------------------------------------------------------- */

static void v_close(dmm_driver_t *self) {
    if (!self) return;
    hp_state_t *s = st_of(self);
    if (s) {
        s->running = false;
        pthread_join(s->reader, NULL);
        if (s->scpi) scpi_close(s->scpi);
        pthread_mutex_destroy(&s->state_lock);
        free(s);
    }
    free(self);
}

static bool v_is_connected(dmm_driver_t *self) { return st_of(self)->connected; }

static void v_read(dmm_driver_t *self, dmm_reading_t *out) {
    hp_state_t *s = st_of(self);
    pthread_mutex_lock(&s->state_lock);
    *out = s->state;
    pthread_mutex_unlock(&s->state_lock);
}

static bool v_set_mode(dmm_driver_t *self, dmm_mode_t mode) {
    if (mode < 0 || mode >= DMM_MODE_COUNT) return false;
    if (!hp_func_keyword(mode)) return false;     /* mode not supported on 3458A */
    hp_state_t *s = st_of(self);
    pthread_mutex_lock(&s->state_lock);
    s->mode_cache = mode;
    s->state.mode = mode;
    pthread_mutex_unlock(&s->state_lock);
    hp_send_function(s, mode, s->range_cache);
    hp_send_nplc(s, nplc_for_rate(s->prof, s->rate_cache));
    return true;
}

static bool v_set_range(dmm_driver_t *self, float range) {
    if (range < 0) return false;
    hp_state_t *s = st_of(self);
    pthread_mutex_lock(&s->state_lock);
    s->range_cache = range;
    s->state.range = range;
    pthread_mutex_unlock(&s->state_lock);
    hp_send_function(s, s->mode_cache, range);
    return true;
}

static bool v_set_rate(dmm_driver_t *self, dmm_rate_t rate) {
    hp_state_t *s = st_of(self);
    pthread_mutex_lock(&s->state_lock);
    s->rate_cache = rate;
    s->state.rate = rate;
    pthread_mutex_unlock(&s->state_lock);
    hp_send_nplc(s, nplc_for_rate(s->prof, rate));
    return true;
}

static void v_get_stats(dmm_driver_t *self, uint32_t *rx, uint32_t *err) {
    hp_state_t *s = st_of(self);
    if (rx)  *rx  = s->rx_count;
    if (err) *err = s->err_count;
}

/* ----- factory (profile-driven) ----------------------------------------- */

static dmm_driver_t *hp_dcl_open(const hp_dcl_profile_t *prof,
                                 const char *port_spec, int default_baud) {
    if (!prof || !port_spec) return NULL;

    scpi_t *scpi = scpi_open(port_spec, default_baud);
    if (!scpi) {
        fprintf(stderr, "hp-dcl: failed to open transport for '%s'\n", port_spec);
        return NULL;
    }

    hp_state_t   *s = calloc(1, sizeof(*s));
    dmm_driver_t *d = calloc(1, sizeof(*d));
    if (!s || !d) { free(s); free(d); scpi_close(scpi); return NULL; }

    s->prof = prof;
    s->scpi = scpi;
    s->running = true;
    s->mode_cache  = DMM_MODE_DC_VOLTS;
    s->range_cache = 0.0f;     /* AUTO */
    s->rate_cache  = DMM_RATE_MEDIUM;
    pthread_mutex_init(&s->state_lock, NULL);
    s->state.mode = DMM_MODE_DC_VOLTS;

    d->state = s;
    d->caps = (dmm_caps_t){
        .model_name             = prof->model_name,
        .display_digits         = prof->display_digits,
        .supports_rate_control  = true,
        .supports_range_control = true,
        .supports_mode = {
            [DMM_MODE_DC_VOLTS]    = true,
            [DMM_MODE_AC_VOLTS]    = true,
            [DMM_MODE_DC_AMPS]     = true,
            [DMM_MODE_AC_AMPS]     = true,
            [DMM_MODE_OHMS_2W]     = true,
            [DMM_MODE_OHMS_4W]     = true,
            [DMM_MODE_FREQUENCY]   = true,
            [DMM_MODE_PERIOD]      = true,
            /* No capacitance / diode / continuity / temperature in this family. */
        },
    };
    d->close        = v_close;
    d->is_connected = v_is_connected;
    d->read         = v_read;
    d->set_mode     = v_set_mode;
    d->set_range    = v_set_range;
    d->set_rate     = v_set_rate;
    d->get_stats    = v_get_stats;

    if (pthread_create(&s->reader, NULL, reader_main, d) != 0) {
        v_close(d);
        return NULL;
    }
    return d;
}

static dmm_driver_t *open_3458a(const char *p, int b) { return hp_dcl_open(&k_prof_3458a, p, b); }
static dmm_driver_t *open_3457a(const char *p, int b) { return hp_dcl_open(&k_prof_3457a, p, b); }

const dmm_driver_factory_t hp_3458a_factory = {
    .id           = "hp-3458a",
    .display_name = "HP 3458A — 8½-digit GPIB reference DMM",
    .description  = "HP-language (DCL), not SCPI. GPIB-only — use prologix:<dev>:<gpib-addr>.",
    .default_baud = 115200,
    .open         = open_3458a,
};

const dmm_driver_factory_t hp_3457a_factory = {
    .id           = "hp-3457a",
    .display_name = "HP 3457A — 6½-digit GPIB DMM (DCL)",
    .description  = "Predecessor to the 3458A; same DCV/ACV/OHM/OHMF command set, 6½ digit.",
    .default_baud = 115200,
    .open         = open_3457a,
};
