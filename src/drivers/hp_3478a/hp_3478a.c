/**
 * HP 3478A — 5½-digit bench DMM, very popular vintage instrument.
 *
 * Wire dialect: HP single-letter codes, NOT SCPI. There is no *IDN?
 * command; identification is via reading the status byte instead.
 *
 * Function codes:
 *     F1 = DC volts
 *     F2 = AC volts
 *     F3 = 2-wire ohms
 *     F4 = 4-wire ohms
 *     F5 = DC current
 *     F6 = AC current
 *     F7 = extended ohms (>30 MΩ)
 *
 * Range codes (interpretation depends on the active function):
 *     R-2  R-1  R0  R1  R2  R3  R4  R5  R6  R7   RA (auto)
 *
 * Trigger:
 *     T1 = internal continuous   T2 = external   T3 = single   T4 = hold   T5 = fast
 *
 * Integration time:
 *     N3 = 3.5-digit, fast        N4 = 4.5-digit, medium       N5 = 5.5-digit, slow
 *
 * Display:
 *     D1 = readout       D2 = constant on display     D3 = text on display
 *
 * Reading transfer: with T1 (continuous) the instrument streams readings
 * as fixed-format ASCII (e.g. "+1.23456E+0\r\n") with EOI on each.
 * We use T3 (single trigger) and explicitly ask for one reading per poll
 * via a bare "T3" command followed by the response.
 *
 * Reach the instrument via Prologix GPIB-USB-HPIB:
 *     --port=prologix:/dev/ttyUSB0:<gpib-addr>
 */

#include "hp_3478a.h"

#include "platform/platform.h"
#include "transport/scpi.h"

#include <math.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define POLL_MS          250
#define QUERY_TIMEOUT_MS 1200
#define SETUP_TIMEOUT_MS 600
#define FUNC_POLL_EVERY  10

typedef struct {
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
} hp78_state_t;

static hp78_state_t *st_of(dmm_driver_t *d) { return (hp78_state_t *)d->state; }

/* Single-letter F-command for `mode`, or NULL if unsupported. */
static const char *func_code(dmm_mode_t m) {
    switch (m) {
        case DMM_MODE_DC_VOLTS: return "F1";
        case DMM_MODE_AC_VOLTS: return "F2";
        case DMM_MODE_OHMS_2W:  return "F3";
        case DMM_MODE_OHMS_4W:  return "F4";
        case DMM_MODE_DC_AMPS:  return "F5";
        case DMM_MODE_AC_AMPS:  return "F6";
        default:                return NULL;
    }
}

/* Map a numeric range value to the appropriate Rn code for the current
 * function. range <= 0 → "RA" (autorange). The exact full-scale per code
 * differs by function; we pick the smallest R that contains the requested
 * range, biased to the common DCV table. Used as a coarse hint; the user
 * can prefer autorange by clicking RANGE off. */
static const char *range_code(dmm_mode_t mode, float range) {
    if (range <= 0) return "RA";
    if (mode == DMM_MODE_DC_VOLTS || mode == DMM_MODE_AC_VOLTS) {
        if (range <= 0.03f)    return "R-2";   /*  30 mV */
        if (range <= 0.3f)     return "R-1";   /* 300 mV */
        if (range <= 3.0f)     return "R0";    /*   3 V  */
        if (range <= 30.0f)    return "R1";    /*  30 V  */
        return "R2";                            /* 300 V  */
    }
    if (mode == DMM_MODE_DC_AMPS || mode == DMM_MODE_AC_AMPS) {
        if (range <= 0.3f)     return "R-1";
        return "R0";                            /*   3 A  */
    }
    if (mode == DMM_MODE_OHMS_2W || mode == DMM_MODE_OHMS_4W) {
        if (range <= 30.0f)        return "R1";
        if (range <= 300.0f)       return "R2";
        if (range <= 3000.0f)      return "R3";
        if (range <= 30000.0f)     return "R4";
        if (range <= 300000.0f)    return "R5";
        if (range <= 3000000.0f)   return "R6";
        return "R7";                            /* 30 MΩ */
    }
    return "RA";
}

static const char *rate_code(dmm_rate_t r) {
    switch (r) {
        case DMM_RATE_SLOW:   return "N5";    /* 5.5 digits */
        case DMM_RATE_FAST:   return "N3";    /* 3.5 digits */
        case DMM_RATE_MEDIUM:
        default:              return "N4";    /* 4.5 digits */
    }
}

static void send_raw(hp78_state_t *s, const char *cmd) {
    char buf[32];
    if (!scpi_query(s->scpi, cmd, buf, sizeof(buf), SETUP_TIMEOUT_MS)) {
        /* Empty reply expected on setup commands. */
    }
}

static void apply_mode_range(hp78_state_t *s, dmm_mode_t mode, float range) {
    const char *f = func_code(mode);
    if (!f) return;
    send_raw(s, f);
    send_raw(s, range_code(mode, range));
}

static void *reader_main(void *arg) {
    dmm_driver_t  *d = (dmm_driver_t *)arg;
    hp78_state_t  *s = st_of(d);

    /* Initial setup: select DCV / auto / medium rate / single-trigger,
     * display-on. The instrument doesn't acknowledge these, so the
     * Prologix auto-read just times out (handled by send_raw). */
    apply_mode_range(s, s->mode_cache, s->range_cache);
    send_raw(s, rate_code(s->rate_cache));
    send_raw(s, "T4");        /* trigger hold; we'll explicitly T3 each cycle */
    send_raw(s, "D1");        /* normal display */
    fprintf(stderr, "hp-3478a: configured (no *IDN? on this model)\n");
    s->connected = true;

    int func_counter = 0;
    while (s->running) {
        /* Trigger a single measurement: "T3" + read response. */
        char mbuf[64];
        if (scpi_query(s->scpi, "T3", mbuf, sizeof(mbuf), QUERY_TIMEOUT_MS)) {
            char *end = NULL;
            float v = strtof(mbuf, &end);
            if (end != mbuf) {
                bool ol = (fabsf(v) > 1.0e9f);   /* 3478A reports overload as huge value */
                pthread_mutex_lock(&s->state_lock);
                s->state.value        = v;
                s->state.overload     = ol;
                s->state.valid        = true;
                s->state.timestamp_ms = pl_now_ms();
                s->state.range        = s->range_cache;
                s->state.rate         = s->rate_cache;
                s->state.mode         = s->mode_cache;
                pthread_mutex_unlock(&s->state_lock);
                s->rx_count++;
            } else {
                s->err_count++;
            }
        } else {
            s->err_count++;
        }

        /* Periodically reapply mode/range/rate in case the user pressed
         * a button on the front panel (the 3478A has no FUNC? query, so
         * we just re-assert our cached settings). */
        if (++func_counter >= FUNC_POLL_EVERY) {
            func_counter = 0;
            apply_mode_range(s, s->mode_cache, s->range_cache);
            send_raw(s, rate_code(s->rate_cache));
        }

        pl_sleep_ms(POLL_MS);
    }
    return NULL;
}

static void v_close(dmm_driver_t *self) {
    if (!self) return;
    hp78_state_t *s = st_of(self);
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
    hp78_state_t *s = st_of(self);
    pthread_mutex_lock(&s->state_lock);
    *out = s->state;
    pthread_mutex_unlock(&s->state_lock);
}

static bool v_set_mode(dmm_driver_t *self, dmm_mode_t mode) {
    if (!func_code(mode)) return false;
    hp78_state_t *s = st_of(self);
    pthread_mutex_lock(&s->state_lock);
    s->mode_cache = mode;
    s->state.mode = mode;
    pthread_mutex_unlock(&s->state_lock);
    apply_mode_range(s, mode, s->range_cache);
    return true;
}

static bool v_set_range(dmm_driver_t *self, float range) {
    if (range < 0) return false;
    hp78_state_t *s = st_of(self);
    pthread_mutex_lock(&s->state_lock);
    s->range_cache = range;
    s->state.range = range;
    pthread_mutex_unlock(&s->state_lock);
    apply_mode_range(s, s->mode_cache, range);
    return true;
}

static bool v_set_rate(dmm_driver_t *self, dmm_rate_t rate) {
    hp78_state_t *s = st_of(self);
    pthread_mutex_lock(&s->state_lock);
    s->rate_cache = rate;
    s->state.rate = rate;
    pthread_mutex_unlock(&s->state_lock);
    send_raw(s, rate_code(rate));
    return true;
}

static void v_get_stats(dmm_driver_t *self, uint32_t *rx, uint32_t *err) {
    hp78_state_t *s = st_of(self);
    if (rx)  *rx  = s->rx_count;
    if (err) *err = s->err_count;
}

static dmm_driver_t *hp_open(const char *port_spec, int default_baud) {
    if (!port_spec) return NULL;
    scpi_t *scpi = scpi_open(port_spec, default_baud);
    if (!scpi) {
        fprintf(stderr, "hp-3478a: failed to open transport for '%s'\n", port_spec);
        return NULL;
    }

    hp78_state_t  *s = calloc(1, sizeof(*s));
    dmm_driver_t  *d = calloc(1, sizeof(*d));
    if (!s || !d) { free(s); free(d); scpi_close(scpi); return NULL; }

    s->scpi = scpi;
    s->running = true;
    s->mode_cache  = DMM_MODE_DC_VOLTS;
    s->range_cache = 0.0f;
    s->rate_cache  = DMM_RATE_MEDIUM;
    pthread_mutex_init(&s->state_lock, NULL);
    s->state.mode = DMM_MODE_DC_VOLTS;

    d->state = s;
    d->caps = (dmm_caps_t){
        .model_name             = "HP 3478A (5½-digit, F-command DCL)",
        .display_digits         = 5,
        .supports_rate_control  = true,
        .supports_range_control = true,
        .supports_mode = {
            [DMM_MODE_DC_VOLTS] = true,
            [DMM_MODE_AC_VOLTS] = true,
            [DMM_MODE_DC_AMPS]  = true,
            [DMM_MODE_AC_AMPS]  = true,
            [DMM_MODE_OHMS_2W]  = true,
            [DMM_MODE_OHMS_4W]  = true,
            /* No frequency / period / capacitance / diode / continuity / temp. */
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

const dmm_driver_factory_t hp_3478a_factory = {
    .id           = "hp-3478a",
    .display_name = "HP 3478A — 5½-digit GPIB DMM (F-commands)",
    .description  = "Single-letter F/R/N/T command set. GPIB-only — use prologix:<dev>:<gpib-addr>.",
    .default_baud = 115200,
    .open         = hp_open,
};
