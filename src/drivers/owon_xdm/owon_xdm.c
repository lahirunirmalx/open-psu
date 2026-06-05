/**
 * OWON XDM bench-DMM driver - SCPI over USB-serial.
 *
 * Wire protocol (matches what markusdd/rusty_meter sends):
 *
 *   *IDN?               → "OWON,XDM1041,<serial>,<fw>\r\n"
 *   SYST:REM            (lock front panel on open)
 *   SYST:LOC            (release on close)
 *   RATE S|M|F          (slow / medium / fast sample rate)
 *   CONF:VOLT:DC <r>    (r = AUTO | 50E-3 | 500E-3 | 5 | 50 | 500 | 1000)
 *   CONF:VOLT:AC <r>    (r = AUTO | 500E-3 | 5 | 50 | 500 | 750)
 *   CONF:CURR:DC <r>    (r = AUTO | 500E-6 | 5E-3 | 50E-3 | 500E-3 | 5 | 10)
 *   CONF:CURR:AC <r>    same ranges
 *   CONF:RES  <r>       (AUTO | 500 | 5E3 | 50E3 | 500E3 | 5E6 | 50E6)
 *   CONF:FRES <r>       (4-wire, same ranges as RES - XDM2041+ only)
 *   CONF:CAP  <r>       (AUTO | 50E-9 | 500E-9 | 5E-6 | 50E-6 | 500E-6 | 5E-3 | 50E-3)
 *   CONF:FREQ           (no range argument)
 *   CONF:PER
 *   CONF:DIOD
 *   CONF:CONT
 *   CONF:TEMP:RTD PT100 (also KITS90)
 *
 *   MEAS?               → "<float>\r\n"   (or a special token on overload)
 *   FUNC?               → quoted function string, e.g. "VOLT" | "VOLT AC" |
 *                         "CURR" | "CURR AC" | "RES" | "FRES" | "CAP" |
 *                         "FREQ" | "PER" | "TEMP" | "DIOD" | "CONT"
 *
 * Firmware quirk: on XDM1041/1241 firmware < 4.3.0, DIOD and CONT mode
 * labels are swapped in the FUNC? reply. We detect from *IDN? and undo
 * the swap when parsing.
 */

#include "owon_xdm.h"

#include "platform/platform.h"
#include "serial_port.h"

#include <ctype.h>
#include <math.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define POLL_MS          200
#define IDN_TIMEOUT_MS   800
#define QUERY_TIMEOUT_MS 500
#define FUNC_POLL_EVERY  5    /* re-read FUNC? every Nth iteration */

typedef struct {
    serial_port_t  *sp;
    pthread_t       reader;
    volatile bool   running;
    volatile bool   connected;

    pthread_mutex_t state_lock;
    dmm_reading_t   state;

    /* Cached settings (we don't try to query them back - write-through). */
    dmm_mode_t mode_cache;
    float      range_cache;
    dmm_rate_t rate_cache;

    bool       swap_diod_cont;   /* set if IDN reports firmware < 4.3.0 */

    volatile uint32_t rx_count;
    volatile uint32_t err_count;
} xdm_state_t;

static xdm_state_t *st_of(dmm_driver_t *d) { return (xdm_state_t *)d->state; }

#define now_ms() pl_now_ms()

/* ---- IDN parsing for firmware version detection ---- */

static void detect_idn_quirk(xdm_state_t *s, const char *idn) {
    /* Expected: "OWON,XDM1041,<serial>,Vx.y.z\r\n" */
    s->swap_diod_cont = false;
    const char *parts[5] = {0};
    int n = 0;
    parts[0] = idn;
    for (const char *p = idn; *p && n < 4; p++) {
        if (*p == ',') {
            parts[++n] = p + 1;
        }
    }
    if (n < 3) return;
    const char *model = parts[1];
    const char *fw    = parts[3];
    bool is_xdm1x41 = (strncmp(model, "XDM1041", 7) == 0 ||
                      strncmp(model, "XDM1241", 7) == 0);
    if (!is_xdm1x41 || !fw || !*fw) return;
    /* fw begins with 'V' or 'v'; skip optional prefix */
    while (*fw && !isdigit((unsigned char)*fw)) fw++;
    int major = atoi(fw);
    const char *dot = strchr(fw, '.');
    int minor = dot ? atoi(dot + 1) : 0;
    if (major < 4 || (major == 4 && minor < 3)) {
        s->swap_diod_cont = true;
        fprintf(stderr,
                "owon-xdm: firmware v%d.%d detected, applying DIOD/CONT swap\n",
                major, minor);
    }
}

/* ---- function-string → dmm_mode_t ---- */

static dmm_mode_t parse_func_reply(xdm_state_t *s, const char *resp) {
    /* OWON returns the function name with optional surrounding quotes and
     * "AC" suffix for AC variants. Strip whitespace + quotes first. */
    char buf[32] = {0};
    size_t n = 0;
    for (const char *p = resp; *p && n < sizeof(buf) - 1; p++) {
        if (*p == '"' || *p == '\r' || *p == '\n') continue;
        buf[n++] = *p;
    }
    /* Trim trailing space */
    while (n > 0 && (buf[n - 1] == ' ' || buf[n - 1] == '\t')) buf[--n] = '\0';

    if (strcmp(buf, "VOLT")     == 0) return DMM_MODE_DC_VOLTS;
    if (strcmp(buf, "VOLT AC")  == 0) return DMM_MODE_AC_VOLTS;
    if (strcmp(buf, "CURR")     == 0) return DMM_MODE_DC_AMPS;
    if (strcmp(buf, "CURR AC")  == 0) return DMM_MODE_AC_AMPS;
    if (strcmp(buf, "RES")      == 0) return DMM_MODE_OHMS_2W;
    if (strcmp(buf, "FRES")     == 0) return DMM_MODE_OHMS_4W;
    if (strcmp(buf, "CAP")      == 0) return DMM_MODE_CAPACITANCE;
    if (strcmp(buf, "FREQ")     == 0) return DMM_MODE_FREQUENCY;
    if (strcmp(buf, "PER")      == 0) return DMM_MODE_PERIOD;
    if (strcmp(buf, "TEMP")     == 0) return DMM_MODE_TEMPERATURE;
    if (strcmp(buf, "DIOD")     == 0) return s->swap_diod_cont ? DMM_MODE_CONTINUITY : DMM_MODE_DIODE;
    if (strcmp(buf, "CONT")     == 0) return s->swap_diod_cont ? DMM_MODE_DIODE      : DMM_MODE_CONTINUITY;
    return DMM_MODE_DC_VOLTS;
}

/* ---- mode → CONF: command + range encoding ---- */

static const char *range_str(dmm_mode_t mode, float r) {
    if (r <= 0) return "AUTO";
    /* OWON range string list, choose the smallest range >= |value| per mode.
     * Calling code uses these literal strings to match the instrument's
     * accepted values. */
    static const struct { dmm_mode_t mode; float v; const char *s; } table[] = {
        {DMM_MODE_DC_VOLTS, 0.050f, "50E-3"},   {DMM_MODE_DC_VOLTS, 0.500f, "500E-3"},
        {DMM_MODE_DC_VOLTS, 5.0f,   "5"},        {DMM_MODE_DC_VOLTS, 50.0f,  "50"},
        {DMM_MODE_DC_VOLTS, 500.0f, "500"},      {DMM_MODE_DC_VOLTS, 1000.0f,"1000"},

        {DMM_MODE_AC_VOLTS, 0.500f, "500E-3"},   {DMM_MODE_AC_VOLTS, 5.0f,   "5"},
        {DMM_MODE_AC_VOLTS, 50.0f,  "50"},       {DMM_MODE_AC_VOLTS, 500.0f, "500"},
        {DMM_MODE_AC_VOLTS, 750.0f, "750"},

        {DMM_MODE_DC_AMPS,  0.0005f,"500E-6"},   {DMM_MODE_DC_AMPS,  0.005f, "5E-3"},
        {DMM_MODE_DC_AMPS,  0.05f,  "50E-3"},    {DMM_MODE_DC_AMPS,  0.5f,   "500E-3"},
        {DMM_MODE_DC_AMPS,  5.0f,   "5"},        {DMM_MODE_DC_AMPS,  10.0f,  "10"},

        {DMM_MODE_AC_AMPS,  0.0005f,"500E-6"},   {DMM_MODE_AC_AMPS,  0.005f, "5E-3"},
        {DMM_MODE_AC_AMPS,  0.05f,  "50E-3"},    {DMM_MODE_AC_AMPS,  0.5f,   "500E-3"},
        {DMM_MODE_AC_AMPS,  5.0f,   "5"},        {DMM_MODE_AC_AMPS,  10.0f,  "10"},

        {DMM_MODE_OHMS_2W,   500.0f,    "500"},  {DMM_MODE_OHMS_2W,    5e3f, "5E3"},
        {DMM_MODE_OHMS_2W,   50e3f,     "50E3"}, {DMM_MODE_OHMS_2W,   500e3f,"500E3"},
        {DMM_MODE_OHMS_2W,   5e6f,      "5E6"},  {DMM_MODE_OHMS_2W,    50e6f,"50E6"},

        {DMM_MODE_OHMS_4W,   500.0f,    "500"},  {DMM_MODE_OHMS_4W,    5e3f, "5E3"},
        {DMM_MODE_OHMS_4W,   50e3f,     "50E3"}, {DMM_MODE_OHMS_4W,   500e3f,"500E3"},
        {DMM_MODE_OHMS_4W,   5e6f,      "5E6"},  {DMM_MODE_OHMS_4W,    50e6f,"50E6"},

        {DMM_MODE_CAPACITANCE, 50e-9f, "50E-9"},  {DMM_MODE_CAPACITANCE, 500e-9f,"500E-9"},
        {DMM_MODE_CAPACITANCE, 5e-6f,  "5E-6"},   {DMM_MODE_CAPACITANCE, 50e-6f, "50E-6"},
        {DMM_MODE_CAPACITANCE, 500e-6f,"500E-6"}, {DMM_MODE_CAPACITANCE, 5e-3f,  "5E-3"},
        {DMM_MODE_CAPACITANCE, 50e-3f, "50E-3"},
    };
    for (size_t i = 0; i < sizeof(table) / sizeof(table[0]); i++) {
        if (table[i].mode == mode && r <= table[i].v) return table[i].s;
    }
    return "AUTO";   /* fall back if value exceeds all listed ranges */
}

static bool send_mode_cmd(xdm_state_t *s, dmm_mode_t mode, float range) {
    char cmd[64];
    const char *rs = range_str(mode, range);
    switch (mode) {
        case DMM_MODE_DC_VOLTS:    snprintf(cmd, sizeof(cmd), "CONF:VOLT:DC %s", rs);  break;
        case DMM_MODE_AC_VOLTS:    snprintf(cmd, sizeof(cmd), "CONF:VOLT:AC %s", rs);  break;
        case DMM_MODE_DC_AMPS:     snprintf(cmd, sizeof(cmd), "CONF:CURR:DC %s", rs);  break;
        case DMM_MODE_AC_AMPS:     snprintf(cmd, sizeof(cmd), "CONF:CURR:AC %s", rs);  break;
        case DMM_MODE_OHMS_2W:     snprintf(cmd, sizeof(cmd), "CONF:RES %s",     rs);  break;
        case DMM_MODE_OHMS_4W:     snprintf(cmd, sizeof(cmd), "CONF:FRES %s",    rs);  break;
        case DMM_MODE_CAPACITANCE: snprintf(cmd, sizeof(cmd), "CONF:CAP %s",     rs);  break;
        case DMM_MODE_FREQUENCY:   snprintf(cmd, sizeof(cmd), "CONF:FREQ");            break;
        case DMM_MODE_PERIOD:      snprintf(cmd, sizeof(cmd), "CONF:PER");             break;
        case DMM_MODE_DIODE:       snprintf(cmd, sizeof(cmd), "CONF:DIOD");            break;
        case DMM_MODE_CONTINUITY:  snprintf(cmd, sizeof(cmd), "CONF:CONT");            break;
        case DMM_MODE_TEMPERATURE: snprintf(cmd, sizeof(cmd), "CONF:TEMP:RTD PT100");  break;
        default: return false;
    }
    bool ok = serial_send_line(s->sp, cmd);
    if (!ok) s->err_count++;
    return ok;
}

/* ---- reader thread ---- */

static bool parse_float(const char *resp, float *out, bool *overload) {
    if (!resp || !*resp) return false;
    /* OWON returns ±9.9E37 for OL on most modes; treat values with magnitude
     * over 9e36 as overload. */
    char *end = NULL;
    float v = strtof(resp, &end);
    if (end == resp) return false;
    *out = v;
    *overload = (fabsf(v) > 9.0e36f);
    return true;
}

static void *reader_main(void *arg) {
    dmm_driver_t *d = (dmm_driver_t *)arg;
    xdm_state_t  *s = st_of(d);

    /* Identify the instrument. */
    char idn[160] = {0};
    if (serial_command(s->sp, "*IDN?", idn, sizeof(idn), IDN_TIMEOUT_MS)) {
        fprintf(stderr, "owon-xdm: %s", idn);
        if (strchr(idn, '\n') == NULL) fputc('\n', stderr);
        detect_idn_quirk(s, idn);
        s->connected = true;
        s->rx_count++;
    } else {
        fprintf(stderr, "owon-xdm: *IDN? timed out - continuing anyway\n");
        s->err_count++;
    }

    /* Lock front panel so our writes aren't fought by the user. */
    serial_send_line(s->sp, "SYST:REM");

    int func_counter = 0;
    while (s->running) {
        /* Periodically re-read the function (so a front-panel mode change is
         * reflected even though we lock the keypad). */
        if (func_counter == 0) {
            char fbuf[64];
            if (serial_command(s->sp, "FUNC?", fbuf, sizeof(fbuf), QUERY_TIMEOUT_MS)) {
                s->rx_count++;
                dmm_mode_t m = parse_func_reply(s, fbuf);
                pthread_mutex_lock(&s->state_lock);
                s->state.mode = m;
                s->mode_cache = m;
                pthread_mutex_unlock(&s->state_lock);
            } else {
                s->err_count++;
            }
        }
        func_counter = (func_counter + 1) % FUNC_POLL_EVERY;

        /* Read the measurement. */
        char mbuf[64];
        if (serial_command(s->sp, "MEAS?", mbuf, sizeof(mbuf), QUERY_TIMEOUT_MS)) {
            float v = 0.0f;
            bool ol = false;
            if (parse_float(mbuf, &v, &ol)) {
                s->rx_count++;
                pthread_mutex_lock(&s->state_lock);
                s->state.value        = v;
                s->state.overload     = ol;
                s->state.valid        = true;
                s->state.timestamp_ms = now_ms();
                pthread_mutex_unlock(&s->state_lock);
                s->connected = true;
            } else {
                s->err_count++;
            }
        } else {
            s->err_count++;
        }

        /* Pace the loop. */
        for (int i = 0; i < POLL_MS / 20 && s->running; i++)
            pl_sleep_ms(20);
    }
    return NULL;
}

/* ---- vtable ---- */

static void v_close(dmm_driver_t *self) {
    if (!self) return;
    xdm_state_t *s = st_of(self);
    if (s) {
        s->running = false;
        pthread_join(s->reader, NULL);
        if (s->sp) {
            serial_send_line(s->sp, "SYST:LOC");   /* release the front panel */
            serial_close(s->sp);
        }
        pthread_mutex_destroy(&s->state_lock);
        free(s);
    }
    free(self);
}

static bool v_is_connected(dmm_driver_t *self) { return st_of(self)->connected; }

static void v_read(dmm_driver_t *self, dmm_reading_t *out) {
    xdm_state_t *s = st_of(self);
    pthread_mutex_lock(&s->state_lock);
    *out = s->state;
    pthread_mutex_unlock(&s->state_lock);
}

static bool v_set_mode(dmm_driver_t *self, dmm_mode_t mode) {
    if (mode < 0 || mode >= DMM_MODE_COUNT) return false;
    xdm_state_t *s = st_of(self);
    pthread_mutex_lock(&s->state_lock);
    s->mode_cache = mode;
    s->state.mode = mode;
    pthread_mutex_unlock(&s->state_lock);
    return send_mode_cmd(s, mode, s->range_cache);
}

static bool v_set_range(dmm_driver_t *self, float range) {
    if (range < 0) return false;
    xdm_state_t *s = st_of(self);
    pthread_mutex_lock(&s->state_lock);
    s->range_cache = range;
    s->state.range = range;
    pthread_mutex_unlock(&s->state_lock);
    return send_mode_cmd(s, s->mode_cache, range);
}

static bool v_set_rate(dmm_driver_t *self, dmm_rate_t rate) {
    xdm_state_t *s = st_of(self);
    const char *r = "M";
    switch (rate) {
        case DMM_RATE_SLOW:   r = "S"; break;
        case DMM_RATE_MEDIUM: r = "M"; break;
        case DMM_RATE_FAST:   r = "F"; break;
    }
    char buf[16];
    snprintf(buf, sizeof(buf), "RATE %s", r);
    bool ok = serial_send_line(s->sp, buf);
    if (ok) {
        pthread_mutex_lock(&s->state_lock);
        s->rate_cache = rate;
        s->state.rate = rate;
        pthread_mutex_unlock(&s->state_lock);
    }
    return ok;
}

static void v_get_stats(dmm_driver_t *self, uint32_t *rx, uint32_t *err) {
    xdm_state_t *s = st_of(self);
    if (rx)  *rx  = s->rx_count;
    if (err) *err = s->err_count;
}

/* ---- factory ---- */

static dmm_driver_t *xdm_open(const char *device, int baud) {
    if (!device) return NULL;
    if (baud <= 0) baud = 115200;

    serial_port_t *sp = serial_open(device, baud);
    if (!sp) return NULL;

    xdm_state_t  *s = calloc(1, sizeof(*s));
    dmm_driver_t *d = calloc(1, sizeof(*d));
    if (!s || !d) { free(s); free(d); serial_close(sp); return NULL; }

    s->sp = sp;
    s->running = true;
    s->mode_cache  = DMM_MODE_DC_VOLTS;
    s->range_cache = 0.0f;
    s->rate_cache  = DMM_RATE_MEDIUM;
    pthread_mutex_init(&s->state_lock, NULL);
    s->state.mode = DMM_MODE_DC_VOLTS;

    d->state = s;
    d->caps  = (dmm_caps_t){
        .model_name             = "OWON XDM-series (SCPI)",
        .supports_rate_control  = true,
        .supports_range_control = true,
        .display_digits         = 5,
    };
    /* Modes supported across the XDM1041/1241/2041 family - 4-wire ohms only
     * on XDM2041+, but we expose it and let the instrument NAK if absent. */
    d->caps.supports_mode[DMM_MODE_DC_VOLTS]    = true;
    d->caps.supports_mode[DMM_MODE_AC_VOLTS]    = true;
    d->caps.supports_mode[DMM_MODE_DC_AMPS]     = true;
    d->caps.supports_mode[DMM_MODE_AC_AMPS]     = true;
    d->caps.supports_mode[DMM_MODE_OHMS_2W]     = true;
    d->caps.supports_mode[DMM_MODE_OHMS_4W]     = true;
    d->caps.supports_mode[DMM_MODE_CAPACITANCE] = true;
    d->caps.supports_mode[DMM_MODE_FREQUENCY]   = true;
    d->caps.supports_mode[DMM_MODE_PERIOD]      = true;
    d->caps.supports_mode[DMM_MODE_DIODE]       = true;
    d->caps.supports_mode[DMM_MODE_CONTINUITY]  = true;
    d->caps.supports_mode[DMM_MODE_TEMPERATURE] = true;

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

const dmm_driver_factory_t owon_xdm_factory = {
    .id           = "owon-xdm",
    .display_name = "OWON XDM-series DMM (SCPI)",
    .description  = "OWON XDM1041 / XDM1241 / XDM2041 (+ XDM3000 family if compatible) over USB serial",
    .default_baud = 115200,
    .open         = xdm_open,
};
