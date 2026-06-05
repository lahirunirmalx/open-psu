/**
 * Generic SCPI DMM driver - one implementation, many profiles.
 *
 * Wire commands at a glance (Keysight 34401A canonical set; other vendors
 * mostly identical):
 *
 *   *IDN?                        identification
 *   CONF:VOLT:DC  <range>        configure for DC volts
 *   CONF:VOLT:AC  <range>        AC volts
 *   CONF:CURR:DC  <range>        DC current
 *   CONF:CURR:AC  <range>        AC current
 *   CONF:RES      <range>        2-wire ohms
 *   CONF:FRES     <range>        4-wire ohms
 *   CONF:CAP      <range>        capacitance (34461A+)
 *   CONF:FREQ                    frequency
 *   CONF:PER                     period
 *   CONF:DIOD                    diode test
 *   CONF:CONT                    continuity
 *   CONF:TEMP                    temperature (34465A+ / DMM6500)
 *   READ?                        trigger + return one measurement
 *   FUNC?                        quoted current function - e.g. "VOLT" or "VOLT:AC"
 *   <func>:NPLC <0.02|1|10>      integration time (rate control)
 *
 * Fluke 884x default to 34401A-compatible SCPI; Keithley DMM6500 needs
 * SCPI mode (LANG SCPI99). Each profile names the function reply strings
 * the instrument actually returns from FUNC?, including AC-suffix variants
 * across vendors ("VOLT:AC" vs "VOLT AC").
 */

#include "scpi_dmm.h"

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
#define QUERY_TIMEOUT_MS 800
#define FUNC_POLL_EVERY  6   /* re-read FUNC? every Nth iteration */

/* Compile-time profile for an SCPI DMM model. */
typedef struct {
    const char *model_name;
    int         display_digits;       /* 4 / 5 / 6 / 7 / 8 (½) */
    bool        supports[DMM_MODE_COUNT];

    /* Per-mode CONF: command with a single "%s" for the range token (or
     * empty if the command takes none). NULL = mode not supported. */
    const char *cmd_conf[DMM_MODE_COUNT];

    /* Function name used in <func>:NPLC commands. NULL = no NPLC for this
     * mode. Example: "VOLT:DC" → sent as "VOLT:DC:NPLC 10". */
    const char *func_nplc[DMM_MODE_COUNT];

    /* FUNC? reply strings to recognise (case-sensitive, after stripping
     * quotes/whitespace). The alt variant covers vendor punctuation drift
     * - e.g. Keysight returns "VOLT:AC", Fluke historically "VOLT AC". */
    const char *func_reply    [DMM_MODE_COUNT];
    const char *func_reply_alt[DMM_MODE_COUNT];

    /* SCPI-flavour overrides (defaults shown if NULL): */
    const char *cmd_func_query;    /* "FUNC?" by default; DMM6500 wants "SENS:FUNC?" */
    const char *cmd_read;          /* "READ?" by default */

    /* NPLC values for SLOW / MEDIUM / FAST rate buttons. */
    float nplc_slow;
    float nplc_medium;
    float nplc_fast;

    /* Optional NULL-terminated array sent once after *IDN?. Used e.g. for
     * Keithley DMM6500 (LANG SCPI99) and to disable beeper. */
    const char *const *init_commands;
} scpi_dmm_profile_t;

/* ----- profile table ------------------------------------------------------ */

/* Keysight 34401A (and clones - Fluke 884x default to this command set). */
#define KEYSIGHT_CLASSIC_CONF \
    [DMM_MODE_DC_VOLTS]    = "CONF:VOLT:DC %s",   \
    [DMM_MODE_AC_VOLTS]    = "CONF:VOLT:AC %s",   \
    [DMM_MODE_DC_AMPS]     = "CONF:CURR:DC %s",   \
    [DMM_MODE_AC_AMPS]     = "CONF:CURR:AC %s",   \
    [DMM_MODE_OHMS_2W]     = "CONF:RES %s",       \
    [DMM_MODE_OHMS_4W]     = "CONF:FRES %s",      \
    [DMM_MODE_FREQUENCY]   = "CONF:FREQ",         \
    [DMM_MODE_PERIOD]      = "CONF:PER",          \
    [DMM_MODE_DIODE]       = "CONF:DIOD",         \
    [DMM_MODE_CONTINUITY]  = "CONF:CONT"

#define KEYSIGHT_CLASSIC_NPLC \
    [DMM_MODE_DC_VOLTS] = "VOLT:DC",  \
    [DMM_MODE_AC_VOLTS] = "VOLT:AC",  \
    [DMM_MODE_DC_AMPS]  = "CURR:DC",  \
    [DMM_MODE_AC_AMPS]  = "CURR:AC",  \
    [DMM_MODE_OHMS_2W]  = "RES",      \
    [DMM_MODE_OHMS_4W]  = "FRES",     \
    [DMM_MODE_FREQUENCY]= "FREQ",     \
    [DMM_MODE_PERIOD]   = "PER"

#define KEYSIGHT_CLASSIC_FUNC_REPLY \
    [DMM_MODE_DC_VOLTS]    = "VOLT",      \
    [DMM_MODE_AC_VOLTS]    = "VOLT:AC",   \
    [DMM_MODE_DC_AMPS]     = "CURR",      \
    [DMM_MODE_AC_AMPS]     = "CURR:AC",   \
    [DMM_MODE_OHMS_2W]     = "RES",       \
    [DMM_MODE_OHMS_4W]     = "FRES",      \
    [DMM_MODE_FREQUENCY]   = "FREQ",      \
    [DMM_MODE_PERIOD]      = "PER",       \
    [DMM_MODE_DIODE]       = "DIOD",      \
    [DMM_MODE_CONTINUITY]  = "CONT"

static const scpi_dmm_profile_t k_keysight_34401a = {
    .model_name     = "Keysight/Agilent/HP 34401A",
    .display_digits = 6,
    .supports = {
        [DMM_MODE_DC_VOLTS]    = true, [DMM_MODE_AC_VOLTS]   = true,
        [DMM_MODE_DC_AMPS]     = true, [DMM_MODE_AC_AMPS]    = true,
        [DMM_MODE_OHMS_2W]     = true, [DMM_MODE_OHMS_4W]    = true,
        [DMM_MODE_FREQUENCY]   = true, [DMM_MODE_PERIOD]     = true,
        [DMM_MODE_DIODE]       = true, [DMM_MODE_CONTINUITY] = true,
    },
    .cmd_conf      = { KEYSIGHT_CLASSIC_CONF },
    .func_nplc     = { KEYSIGHT_CLASSIC_NPLC },
    .func_reply    = { KEYSIGHT_CLASSIC_FUNC_REPLY },
    .nplc_slow = 10.0f, .nplc_medium = 1.0f, .nplc_fast = 0.02f,
};

/* Keysight 34461A - Truevolt entry: classic + capacitance. */
static const scpi_dmm_profile_t k_keysight_34461a = {
    .model_name     = "Keysight 34461A (Truevolt)",
    .display_digits = 6,
    .supports = {
        [DMM_MODE_DC_VOLTS]    = true, [DMM_MODE_AC_VOLTS]   = true,
        [DMM_MODE_DC_AMPS]     = true, [DMM_MODE_AC_AMPS]    = true,
        [DMM_MODE_OHMS_2W]     = true, [DMM_MODE_OHMS_4W]    = true,
        [DMM_MODE_CAPACITANCE] = true,
        [DMM_MODE_FREQUENCY]   = true, [DMM_MODE_PERIOD]     = true,
        [DMM_MODE_DIODE]       = true, [DMM_MODE_CONTINUITY] = true,
    },
    .cmd_conf = {
        KEYSIGHT_CLASSIC_CONF,
        [DMM_MODE_CAPACITANCE] = "CONF:CAP %s",
    },
    .func_nplc  = { KEYSIGHT_CLASSIC_NPLC },
    .func_reply = {
        KEYSIGHT_CLASSIC_FUNC_REPLY,
        [DMM_MODE_CAPACITANCE] = "CAP",
    },
    .nplc_slow = 10.0f, .nplc_medium = 1.0f, .nplc_fast = 0.02f,
};

/* Keysight 34465A - Truevolt mid: + temperature. Same 6½ digit. */
static const scpi_dmm_profile_t k_keysight_34465a = {
    .model_name     = "Keysight 34465A (Truevolt)",
    .display_digits = 6,
    .supports = {
        [DMM_MODE_DC_VOLTS]    = true, [DMM_MODE_AC_VOLTS]   = true,
        [DMM_MODE_DC_AMPS]     = true, [DMM_MODE_AC_AMPS]    = true,
        [DMM_MODE_OHMS_2W]     = true, [DMM_MODE_OHMS_4W]    = true,
        [DMM_MODE_CAPACITANCE] = true,
        [DMM_MODE_FREQUENCY]   = true, [DMM_MODE_PERIOD]     = true,
        [DMM_MODE_DIODE]       = true, [DMM_MODE_CONTINUITY] = true,
        [DMM_MODE_TEMPERATURE] = true,
    },
    .cmd_conf = {
        KEYSIGHT_CLASSIC_CONF,
        [DMM_MODE_CAPACITANCE] = "CONF:CAP %s",
        [DMM_MODE_TEMPERATURE] = "CONF:TEMP",
    },
    .func_nplc = {
        KEYSIGHT_CLASSIC_NPLC,
        [DMM_MODE_TEMPERATURE] = "TEMP",
    },
    .func_reply = {
        KEYSIGHT_CLASSIC_FUNC_REPLY,
        [DMM_MODE_CAPACITANCE] = "CAP",
        [DMM_MODE_TEMPERATURE] = "TEMP",
    },
    .nplc_slow = 10.0f, .nplc_medium = 1.0f, .nplc_fast = 0.02f,
};

/* Keysight 34470A - Truevolt top: 7½ digit, otherwise like 34465A. */
static const scpi_dmm_profile_t k_keysight_34470a = {
    .model_name     = "Keysight 34470A (Truevolt 7½)",
    .display_digits = 7,
    .supports = {
        [DMM_MODE_DC_VOLTS]    = true, [DMM_MODE_AC_VOLTS]   = true,
        [DMM_MODE_DC_AMPS]     = true, [DMM_MODE_AC_AMPS]    = true,
        [DMM_MODE_OHMS_2W]     = true, [DMM_MODE_OHMS_4W]    = true,
        [DMM_MODE_CAPACITANCE] = true,
        [DMM_MODE_FREQUENCY]   = true, [DMM_MODE_PERIOD]     = true,
        [DMM_MODE_DIODE]       = true, [DMM_MODE_CONTINUITY] = true,
        [DMM_MODE_TEMPERATURE] = true,
    },
    .cmd_conf = {
        KEYSIGHT_CLASSIC_CONF,
        [DMM_MODE_CAPACITANCE] = "CONF:CAP %s",
        [DMM_MODE_TEMPERATURE] = "CONF:TEMP",
    },
    .func_nplc = {
        KEYSIGHT_CLASSIC_NPLC,
        [DMM_MODE_TEMPERATURE] = "TEMP",
    },
    .func_reply = {
        KEYSIGHT_CLASSIC_FUNC_REPLY,
        [DMM_MODE_CAPACITANCE] = "CAP",
        [DMM_MODE_TEMPERATURE] = "TEMP",
    },
    /* 7½-digit unit supports longer integration. */
    .nplc_slow = 100.0f, .nplc_medium = 1.0f, .nplc_fast = 0.02f,
};

/* Fluke 8845A - defaults to 34401A-compatible SCPI mode out of the box.
 * Older firmware may emit "VOLT AC" (space) instead of "VOLT:AC"; both are
 * listed via func_reply_alt. */
static const scpi_dmm_profile_t k_fluke_8845a = {
    .model_name     = "Fluke 8845A (34401A-compat SCPI)",
    .display_digits = 6,
    .supports = {
        [DMM_MODE_DC_VOLTS]    = true, [DMM_MODE_AC_VOLTS]   = true,
        [DMM_MODE_DC_AMPS]     = true, [DMM_MODE_AC_AMPS]    = true,
        [DMM_MODE_OHMS_2W]     = true,
        [DMM_MODE_FREQUENCY]   = true, [DMM_MODE_PERIOD]     = true,
        [DMM_MODE_DIODE]       = true, [DMM_MODE_CONTINUITY] = true,
    },
    .cmd_conf      = { KEYSIGHT_CLASSIC_CONF },
    .func_nplc     = { KEYSIGHT_CLASSIC_NPLC },
    .func_reply    = { KEYSIGHT_CLASSIC_FUNC_REPLY },
    .func_reply_alt = {
        [DMM_MODE_AC_VOLTS] = "VOLT AC",
        [DMM_MODE_AC_AMPS]  = "CURR AC",
    },
    .nplc_slow = 10.0f, .nplc_medium = 1.0f, .nplc_fast = 0.02f,
};

/* Fluke 8846A - 8845A + 4-wire ohms (and a few extras we don't expose yet). */
static const scpi_dmm_profile_t k_fluke_8846a = {
    .model_name     = "Fluke 8846A (34401A-compat SCPI)",
    .display_digits = 6,
    .supports = {
        [DMM_MODE_DC_VOLTS]    = true, [DMM_MODE_AC_VOLTS]   = true,
        [DMM_MODE_DC_AMPS]     = true, [DMM_MODE_AC_AMPS]    = true,
        [DMM_MODE_OHMS_2W]     = true, [DMM_MODE_OHMS_4W]    = true,
        [DMM_MODE_FREQUENCY]   = true, [DMM_MODE_PERIOD]     = true,
        [DMM_MODE_DIODE]       = true, [DMM_MODE_CONTINUITY] = true,
    },
    .cmd_conf      = { KEYSIGHT_CLASSIC_CONF },
    .func_nplc     = { KEYSIGHT_CLASSIC_NPLC },
    .func_reply    = { KEYSIGHT_CLASSIC_FUNC_REPLY },
    .func_reply_alt = {
        [DMM_MODE_AC_VOLTS] = "VOLT AC",
        [DMM_MODE_AC_AMPS]  = "CURR AC",
    },
    .nplc_slow = 10.0f, .nplc_medium = 1.0f, .nplc_fast = 0.02f,
};

/* Keithley 2000 - legacy 6½-digit bench DMM, GPIB-centric. */
static const scpi_dmm_profile_t k_keithley_2000 = {
    .model_name     = "Keithley 2000",
    .display_digits = 6,
    .supports = {
        [DMM_MODE_DC_VOLTS]    = true, [DMM_MODE_AC_VOLTS]   = true,
        [DMM_MODE_DC_AMPS]     = true, [DMM_MODE_AC_AMPS]    = true,
        [DMM_MODE_OHMS_2W]     = true, [DMM_MODE_OHMS_4W]    = true,
        [DMM_MODE_FREQUENCY]   = true, [DMM_MODE_PERIOD]     = true,
        [DMM_MODE_DIODE]       = true, [DMM_MODE_CONTINUITY] = true,
        [DMM_MODE_TEMPERATURE] = true,
    },
    .cmd_conf = {
        KEYSIGHT_CLASSIC_CONF,
        [DMM_MODE_TEMPERATURE] = "CONF:TEMP",
    },
    .func_nplc = {
        KEYSIGHT_CLASSIC_NPLC,
        [DMM_MODE_TEMPERATURE] = "TEMP",
    },
    .func_reply = {
        KEYSIGHT_CLASSIC_FUNC_REPLY,
        [DMM_MODE_TEMPERATURE] = "TEMP",
    },
    .nplc_slow = 10.0f, .nplc_medium = 1.0f, .nplc_fast = 0.1f,   /* 2000 floors at ~0.1 NPLC */
};

/* Keithley DMM6500 - Touch DMM; ensure SCPI mode on connect. */
static const char *const k_dmm6500_init[] = {
    "*LANG SCPI99",        /* switch out of TSP mode if armed */
    NULL,
};

static const scpi_dmm_profile_t k_keithley_dmm6500 = {
    .model_name     = "Keithley DMM6500",
    .display_digits = 6,
    .supports = {
        [DMM_MODE_DC_VOLTS]    = true, [DMM_MODE_AC_VOLTS]   = true,
        [DMM_MODE_DC_AMPS]     = true, [DMM_MODE_AC_AMPS]    = true,
        [DMM_MODE_OHMS_2W]     = true, [DMM_MODE_OHMS_4W]    = true,
        [DMM_MODE_CAPACITANCE] = true,
        [DMM_MODE_FREQUENCY]   = true, [DMM_MODE_PERIOD]     = true,
        [DMM_MODE_DIODE]       = true, [DMM_MODE_CONTINUITY] = true,
        [DMM_MODE_TEMPERATURE] = true,
    },
    .cmd_conf = {
        KEYSIGHT_CLASSIC_CONF,
        [DMM_MODE_CAPACITANCE] = "CONF:CAP %s",
        [DMM_MODE_TEMPERATURE] = "CONF:TEMP",
    },
    .func_nplc = {
        KEYSIGHT_CLASSIC_NPLC,
        [DMM_MODE_TEMPERATURE] = "TEMP",
    },
    .func_reply = {
        KEYSIGHT_CLASSIC_FUNC_REPLY,
        [DMM_MODE_CAPACITANCE] = "CAP",
        [DMM_MODE_TEMPERATURE] = "TEMP",
    },
    .cmd_func_query = "SENS:FUNC?",  /* DMM6500 prefers the longer form in SCPI mode */
    .init_commands  = k_dmm6500_init,
    .nplc_slow = 10.0f, .nplc_medium = 1.0f, .nplc_fast = 0.02f,
};

/* ----- runtime state ------------------------------------------------------ */

typedef struct {
    scpi_t *scpi;
    const scpi_dmm_profile_t *prof;

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
} dmm_state_t;

static dmm_state_t *st_of(dmm_driver_t *d) { return (dmm_state_t *)d->state; }

#define now_ms() pl_now_ms()

static const char *or_default(const char *s, const char *def) {
    return (s && *s) ? s : def;
}

/* Parse a FUNC? reply: strip quotes / surrounding whitespace into out_buf. */
static void clean_func_reply(const char *raw, char *out, size_t outlen) {
    size_t n = 0;
    for (const char *p = raw; *p && n < outlen - 1; p++) {
        if (*p == '"' || *p == '\r' || *p == '\n') continue;
        out[n++] = *p;
    }
    out[n] = '\0';
    /* trim trailing space */
    while (n > 0 && (out[n - 1] == ' ' || out[n - 1] == '\t')) out[--n] = '\0';
    /* trim leading space */
    size_t lead = 0;
    while (out[lead] == ' ') lead++;
    if (lead > 0) memmove(out, out + lead, n - lead + 1);
}

static dmm_mode_t mode_from_func_reply(const scpi_dmm_profile_t *p,
                                       const char *raw, dmm_mode_t fallback) {
    char buf[64];
    clean_func_reply(raw, buf, sizeof(buf));
    for (int m = 0; m < DMM_MODE_COUNT; m++) {
        if (p->func_reply    [m] && strcmp(p->func_reply    [m], buf) == 0) return (dmm_mode_t)m;
        if (p->func_reply_alt[m] && strcmp(p->func_reply_alt[m], buf) == 0) return (dmm_mode_t)m;
    }
    return fallback;
}

/* Apply NPLC for the current mode using the configured rate's value. */
static void apply_rate(dmm_state_t *s) {
    const char *fn = s->prof->func_nplc[s->mode_cache];
    if (!fn) return;
    float nplc;
    switch (s->rate_cache) {
        case DMM_RATE_SLOW:   nplc = s->prof->nplc_slow;   break;
        case DMM_RATE_FAST:   nplc = s->prof->nplc_fast;   break;
        case DMM_RATE_MEDIUM:
        default:              nplc = s->prof->nplc_medium; break;
    }
    char cmd[80];
    snprintf(cmd, sizeof(cmd), "%s:NPLC %g", fn, (double)nplc);
    if (!scpi_send(s->scpi, cmd)) s->err_count++;
}

/* Send CONF: command for `mode` with the supplied range (0 = AUTO). */
static bool send_conf(dmm_state_t *s, dmm_mode_t mode, float range) {
    const char *fmt = s->prof->cmd_conf[mode];
    if (!fmt) return false;
    char cmd[80];
    if (strstr(fmt, "%s")) {
        char rbuf[32];
        if (range <= 0) snprintf(rbuf, sizeof(rbuf), "AUTO");
        else            snprintf(rbuf, sizeof(rbuf), "%g", (double)range);
        snprintf(cmd, sizeof(cmd), fmt, rbuf);
    } else {
        snprintf(cmd, sizeof(cmd), "%s", fmt);
    }
    bool ok = scpi_send(s->scpi, cmd);
    if (!ok) s->err_count++;
    return ok;
}

/* ----- reader thread ------------------------------------------------------ */

static void *reader_main(void *arg) {
    dmm_driver_t *d = (dmm_driver_t *)arg;
    dmm_state_t  *s = st_of(d);
    const char   *func_q = or_default(s->prof->cmd_func_query, "FUNC?");
    const char   *read_q = or_default(s->prof->cmd_read,       "READ?");

    char idn[160] = {0};
    if (scpi_query(s->scpi, "*IDN?", idn, sizeof(idn), IDN_TIMEOUT_MS)) {
        fprintf(stderr, "scpi-dmm: %s says: %s", s->prof->model_name, idn);
        if (strchr(idn, '\n') == NULL) fputc('\n', stderr);
        s->connected = true;
        s->rx_count++;
    } else {
        fprintf(stderr, "scpi-dmm: *IDN? timed out - continuing anyway\n");
        s->err_count++;
    }

    if (s->prof->init_commands) {
        for (const char *const *c = s->prof->init_commands; *c; c++) {
            if (!scpi_send(s->scpi, *c)) s->err_count++;
        }
    }

    int func_counter = 0;
    while (s->running) {
        if (func_counter == 0) {
            char fbuf[64];
            if (scpi_query(s->scpi, func_q, fbuf, sizeof(fbuf), QUERY_TIMEOUT_MS)) {
                s->rx_count++;
                dmm_mode_t m = mode_from_func_reply(s->prof, fbuf, s->mode_cache);
                pthread_mutex_lock(&s->state_lock);
                s->state.mode = m;
                s->mode_cache = m;
                pthread_mutex_unlock(&s->state_lock);
            } else {
                s->err_count++;
            }
        }
        func_counter = (func_counter + 1) % FUNC_POLL_EVERY;

        char mbuf[64];
        if (scpi_query(s->scpi, read_q, mbuf, sizeof(mbuf), QUERY_TIMEOUT_MS)) {
            char *end = NULL;
            float v = strtof(mbuf, &end);
            if (end != mbuf) {
                bool ol = (fabsf(v) > 9.0e36f);   /* SCPI ±9.9E37 overload convention */
                pthread_mutex_lock(&s->state_lock);
                s->state.value        = v;
                s->state.overload     = ol;
                s->state.valid        = true;
                s->state.timestamp_ms = now_ms();
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

        for (int i = 0; i < POLL_MS / 20 && s->running; i++)
            pl_sleep_ms(20);
    }
    return NULL;
}

/* ----- vtable ------------------------------------------------------------- */

static void v_close(dmm_driver_t *self) {
    if (!self) return;
    dmm_state_t *s = st_of(self);
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
    dmm_state_t *s = st_of(self);
    pthread_mutex_lock(&s->state_lock);
    *out = s->state;
    pthread_mutex_unlock(&s->state_lock);
}

static bool v_set_mode(dmm_driver_t *self, dmm_mode_t mode) {
    dmm_state_t *s = st_of(self);
    if (mode < 0 || mode >= DMM_MODE_COUNT) return false;
    if (!s->prof->supports[mode]) return false;
    pthread_mutex_lock(&s->state_lock);
    s->mode_cache = mode;
    s->state.mode = mode;
    pthread_mutex_unlock(&s->state_lock);
    bool ok = send_conf(s, mode, s->range_cache);
    apply_rate(s);    /* re-apply NPLC for the new function */
    return ok;
}

static bool v_set_range(dmm_driver_t *self, float range) {
    dmm_state_t *s = st_of(self);
    if (range < 0) return false;
    pthread_mutex_lock(&s->state_lock);
    s->range_cache = range;
    s->state.range = range;
    pthread_mutex_unlock(&s->state_lock);
    return send_conf(s, s->mode_cache, range);
}

static bool v_set_rate(dmm_driver_t *self, dmm_rate_t rate) {
    dmm_state_t *s = st_of(self);
    pthread_mutex_lock(&s->state_lock);
    s->rate_cache = rate;
    s->state.rate = rate;
    pthread_mutex_unlock(&s->state_lock);
    apply_rate(s);
    return true;
}

static void v_get_stats(dmm_driver_t *self, uint32_t *rx, uint32_t *err) {
    dmm_state_t *s = st_of(self);
    if (rx)  *rx  = s->rx_count;
    if (err) *err = s->err_count;
}

/* ----- factory shared open ------------------------------------------------ */

static dmm_driver_t *scpi_dmm_open(const scpi_dmm_profile_t *prof,
                                   const char *port_spec, int default_baud) {
    if (!prof || !port_spec) return NULL;

    scpi_t *scpi = scpi_open(port_spec, default_baud);
    if (!scpi) {
        fprintf(stderr, "scpi-dmm: failed to open transport for '%s'\n", port_spec);
        return NULL;
    }

    dmm_state_t  *s = calloc(1, sizeof(*s));
    dmm_driver_t *d = calloc(1, sizeof(*d));
    if (!s || !d) { free(s); free(d); scpi_close(scpi); return NULL; }

    s->scpi = scpi;
    s->prof = prof;
    s->running = true;
    s->mode_cache  = DMM_MODE_DC_VOLTS;
    s->range_cache = 0.0f;
    s->rate_cache  = DMM_RATE_MEDIUM;
    pthread_mutex_init(&s->state_lock, NULL);
    s->state.mode = DMM_MODE_DC_VOLTS;

    d->state = s;
    d->caps = (dmm_caps_t){
        .model_name             = prof->model_name,
        .supports_rate_control  = true,
        .supports_range_control = true,
        .display_digits         = prof->display_digits,
    };
    for (int m = 0; m < DMM_MODE_COUNT; m++)
        d->caps.supports_mode[m] = prof->supports[m];

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

/* ----- per-model factory thunks + factory structs ------------------------ */

#define DEF_FACTORY(NAME, ID, DISPLAY, DESC, BAUD, PROFILE)                \
    static dmm_driver_t *open_##NAME(const char *p, int b) {               \
        return scpi_dmm_open(&PROFILE, p, b);                              \
    }                                                                      \
    const dmm_driver_factory_t NAME##_factory = {                          \
        .id           = ID,                                                \
        .display_name = DISPLAY,                                           \
        .description  = DESC,                                              \
        .default_baud = BAUD,                                              \
        .open         = open_##NAME,                                       \
    }

DEF_FACTORY(keysight_34401a, "keysight-34401a",
            "Keysight/Agilent/HP 34401A (SCPI)",
            "Classic 6½-digit DMM, serial or Prologix GPIB.",
            9600, k_keysight_34401a);

DEF_FACTORY(keysight_34461a, "keysight-34461a",
            "Keysight 34461A Truevolt (SCPI)",
            "6½-digit Truevolt entry-class DMM, USB-serial or Prologix GPIB.",
            9600, k_keysight_34461a);

DEF_FACTORY(keysight_34465a, "keysight-34465a",
            "Keysight 34465A Truevolt (SCPI)",
            "6½-digit Truevolt mid-class DMM, USB-serial or Prologix GPIB.",
            9600, k_keysight_34465a);

DEF_FACTORY(keysight_34470a, "keysight-34470a",
            "Keysight 34470A Truevolt 7½ (SCPI)",
            "7½-digit Truevolt high-end DMM, USB-serial or Prologix GPIB.",
            9600, k_keysight_34470a);

DEF_FACTORY(fluke_8845a, "fluke-8845a",
            "Fluke 8845A (34401A-compat SCPI)",
            "Fluke 6½-digit DMM in default 34401A-compat mode. Serial or GPIB.",
            9600, k_fluke_8845a);

DEF_FACTORY(fluke_8846a, "fluke-8846a",
            "Fluke 8846A (34401A-compat SCPI)",
            "Fluke 6½-digit DMM with 4W ohms. Serial, GPIB, or LAN+serial bridge.",
            9600, k_fluke_8846a);

DEF_FACTORY(keithley_2000, "keithley-2000",
            "Keithley 2000 (SCPI)",
            "Legacy 6½-digit bench DMM. Typically GPIB - use prologix:<dev>:<addr>.",
            9600, k_keithley_2000);

DEF_FACTORY(keithley_dmm6500, "keithley-dmm6500",
            "Keithley DMM6500 (SCPI mode)",
            "Touch-screen 6½-digit DMM; auto-switches to SCPI 1999 mode on open.",
            9600, k_keithley_dmm6500);
