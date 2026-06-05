/**
 * Generic SCPI PSU driver - one impl, multiple profiles.
 *
 * Models that put the channel inside the command (Siglent SPD: "CH1:VOLT 5.0")
 * use channel_in_command=true and format strings carrying both %d (channel)
 * and %.3f (value).
 *
 * Models that use a separate channel-select step (Keysight E3631A:
 * "INST:NSEL 1" then "VOLT 5.0") use channel_in_command=false; the driver
 * sends select_channel_fmt first, then commands with no channel placeholder.
 *
 * A reader thread polls each channel for V/A/P every poll_ms; setpoints and
 * output toggles go through immediately. Both share the scpi_t mutex.
 */

#include "scpi_psu.h"

#include "platform/platform.h"
#include "transport/scpi.h"

#include <math.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_CHANNELS 4

typedef struct {
    const char *model_name;
    int   n_channels;
    float v_max[MAX_CHANNELS];
    float i_max[MAX_CHANNELS];

    bool        channel_in_command;
    const char *select_channel_fmt;   /* used when channel_in_command == false */

    /* Value-setting commands. With channel_in_command, contains "%d" and "%.3f".
     * Without, contains only "%.3f"; the channel is implicit via select. */
    const char *set_voltage_fmt;
    const char *set_current_fmt;

    /* Output toggle. With channel_in_command, contains "%d". Without, no specifier. */
    const char *set_output_on_fmt;
    const char *set_output_off_fmt;

    /* Measurement queries. With channel_in_command, contains "%d". Without, no specifier.
     * meas_power_fmt may be NULL → compute as V * A. */
    const char *meas_voltage_fmt;
    const char *meas_current_fmt;
    const char *meas_power_fmt;

    /* Tracking / series/parallel coupling. Both NULL means the driver does
     * not expose set_tracking() and caps.supports_tracking stays false.
     * If non-NULL the strings have no format specifiers - they are sent
     * verbatim (no per-channel state, this is a global command).
     *
     * For Siglent SPD3303 series we send OUTP:TRACK 1 (series tracking ON)
     * and OUTP:TRACK 0 (back to independent). The instrument must already
     * be in its tracking-capable mode (front-panel "Series"/"Parallel"
     * selection on older firmware). */
    const char *set_tracking_on;
    const char *set_tracking_off;

    /* Optional NULL-terminated array of commands sent verbatim after *IDN?,
     * before the reader's first poll. Used e.g. to enable the master output
     * on R&S HMP (OUTP:GEN ON) so per-channel OUTP:SEL ON has any effect. */
    const char *const *init_commands;
} scpi_psu_profile_t;

/* ---- profile table ---- */

static const scpi_psu_profile_t k_siglent_spd3303 = {
    .model_name = "Siglent SPD3303",
    .n_channels = 2,
    .v_max = {32.0f, 32.0f},
    .i_max = {3.2f,  3.2f},
    .channel_in_command = true,
    .set_voltage_fmt    = "CH%d:VOLT %.3f",
    .set_current_fmt    = "CH%d:CURR %.3f",
    .set_output_on_fmt  = "OUTP CH%d,ON",
    .set_output_off_fmt = "OUTP CH%d,OFF",
    .meas_voltage_fmt   = "MEAS:VOLT? CH%d",
    .meas_current_fmt   = "MEAS:CURR? CH%d",
    .meas_power_fmt     = "MEAS:POWE? CH%d",
    /* OUTP:TRACK <0|1|2> - 0=independent, 1=series, 2=parallel.
     * We toggle series-track on, independent off; user picks parallel via
     * the front panel if needed. Requires the instrument to be in a
     * tracking-capable wiring/mode. */
    .set_tracking_on    = "OUTP:TRACK 1",
    .set_tracking_off   = "OUTP:TRACK 0",
};

static const scpi_psu_profile_t k_keysight_e3631a = {
    .model_name = "Keysight E3631A",
    .n_channels = 3,
    .v_max = {6.0f, 25.0f, 25.0f},   /* P6V, P25V, N25V (absolute) */
    .i_max = {5.0f,  1.0f,  1.0f},
    .channel_in_command = false,
    .select_channel_fmt = "INST:NSEL %d",
    .set_voltage_fmt    = "VOLT %.3f",
    .set_current_fmt    = "CURR %.3f",
    .set_output_on_fmt  = "OUTP ON",
    .set_output_off_fmt = "OUTP OFF",
    .meas_voltage_fmt   = "MEAS:VOLT?",
    .meas_current_fmt   = "MEAS:CURR?",
    .meas_power_fmt     = NULL,
};

static const scpi_psu_profile_t k_keysight_e3633a = {
    .model_name = "Keysight E3633A",
    .n_channels = 1,
    .v_max = {20.0f},   /* high range; low range is 8V */
    .i_max = {10.0f},
    .channel_in_command = false,
    .select_channel_fmt = NULL,
    .set_voltage_fmt    = "VOLT %.3f",
    .set_current_fmt    = "CURR %.3f",
    .set_output_on_fmt  = "OUTP ON",
    .set_output_off_fmt = "OUTP OFF",
    .meas_voltage_fmt   = "MEAS:VOLT?",
    .meas_current_fmt   = "MEAS:CURR?",
    .meas_power_fmt     = NULL,
};

static const scpi_psu_profile_t k_keysight_e3634a = {
    .model_name = "Keysight E3634A",
    .n_channels = 1,
    .v_max = {25.0f},
    .i_max = {7.0f},
    .channel_in_command = false,
    .select_channel_fmt = NULL,
    .set_voltage_fmt    = "VOLT %.3f",
    .set_current_fmt    = "CURR %.3f",
    .set_output_on_fmt  = "OUTP ON",
    .set_output_off_fmt = "OUTP OFF",
    .meas_voltage_fmt   = "MEAS:VOLT?",
    .meas_current_fmt   = "MEAS:CURR?",
    .meas_power_fmt     = NULL,
};

static const scpi_psu_profile_t k_keysight_e3645a = {
    .model_name = "Keysight E3645A",
    .n_channels = 1,
    .v_max = {20.0f},
    .i_max = {5.0f},
    .channel_in_command = false,
    .select_channel_fmt = NULL,
    .set_voltage_fmt    = "VOLT %.3f",
    .set_current_fmt    = "CURR %.3f",
    .set_output_on_fmt  = "OUTP ON",
    .set_output_off_fmt = "OUTP OFF",
    .meas_voltage_fmt   = "MEAS:VOLT?",
    .meas_current_fmt   = "MEAS:CURR?",
    .meas_power_fmt     = NULL,
};

/* ----- Rigol DP800 family -------------------------------------------------
 * Programming guide: same scheme as Siglent SPD - channel in command, with
 * [:SOUR<n>]:VOLT for set and :OUTP CH<n>,ON / :MEAS:VOLT? CH<n> for IO.
 * Verified against the Rigol DP800 Programming Guide command shape. */

static const scpi_psu_profile_t k_rigol_dp832 = {
    .model_name = "Rigol DP832",
    .n_channels = 3,
    .v_max = {30.0f, 30.0f,  5.2f},   /* CH1 30V/3A, CH2 30V/3A, CH3 5V/3A */
    .i_max = { 3.2f,  3.2f,  3.2f},
    .channel_in_command = true,
    .set_voltage_fmt    = ":SOUR%d:VOLT %.3f",
    .set_current_fmt    = ":SOUR%d:CURR %.3f",
    .set_output_on_fmt  = ":OUTP CH%d,ON",
    .set_output_off_fmt = ":OUTP CH%d,OFF",
    .meas_voltage_fmt   = ":MEAS:VOLT? CH%d",
    .meas_current_fmt   = ":MEAS:CURR? CH%d",
    .meas_power_fmt     = ":MEAS:POWE? CH%d",
};

/* DP832A is electrically identical from the SCPI side; we register it as a
 * separate factory so the launcher labels match the front-panel sticker. */
static const scpi_psu_profile_t k_rigol_dp832a = {
    .model_name = "Rigol DP832A",
    .n_channels = 3,
    .v_max = {30.0f, 30.0f,  5.2f},
    .i_max = { 3.2f,  3.2f,  3.2f},
    .channel_in_command = true,
    .set_voltage_fmt    = ":SOUR%d:VOLT %.4f",   /* DP832A has higher resolution */
    .set_current_fmt    = ":SOUR%d:CURR %.4f",
    .set_output_on_fmt  = ":OUTP CH%d,ON",
    .set_output_off_fmt = ":OUTP CH%d,OFF",
    .meas_voltage_fmt   = ":MEAS:VOLT? CH%d",
    .meas_current_fmt   = ":MEAS:CURR? CH%d",
    .meas_power_fmt     = ":MEAS:POWE? CH%d",
};

/* DP811 / DP811A - single output, 0..20 V / 0..10 A (or 0..40 V / 0..5 A
 * in low-current range; we expose the 20V/10A nominal range). */
static const scpi_psu_profile_t k_rigol_dp811 = {
    .model_name = "Rigol DP811",
    .n_channels = 1,
    .v_max = {20.0f},
    .i_max = {10.0f},
    .channel_in_command = false,
    .select_channel_fmt = NULL,           /* single channel */
    .set_voltage_fmt    = ":VOLT %.3f",
    .set_current_fmt    = ":CURR %.3f",
    .set_output_on_fmt  = ":OUTP ON",
    .set_output_off_fmt = ":OUTP OFF",
    .meas_voltage_fmt   = ":MEAS:VOLT?",
    .meas_current_fmt   = ":MEAS:CURR?",
    .meas_power_fmt     = ":MEAS:POWE?",
};

static const scpi_psu_profile_t k_rigol_dp711 = {
    .model_name = "Rigol DP711",
    .n_channels = 1,
    .v_max = {30.0f},
    .i_max = { 5.0f},
    .channel_in_command = false,
    .select_channel_fmt = NULL,
    .set_voltage_fmt    = ":VOLT %.3f",
    .set_current_fmt    = ":CURR %.3f",
    .set_output_on_fmt  = ":OUTP ON",
    .set_output_off_fmt = ":OUTP OFF",
    .meas_voltage_fmt   = ":MEAS:VOLT?",
    .meas_current_fmt   = ":MEAS:CURR?",
    .meas_power_fmt     = ":MEAS:POWE?",
};

/* ----- Rohde & Schwarz HMP / NGE families ---------------------------------
 * Per the HMP / NGE programming manuals: INST OUTn selects the active
 * output, then VOLT/CURR/OUTP:SEL act on it. A master output gate is
 * controlled via OUTP:GEN - we enable it once via init_commands so the
 * per-channel selects have effect. */

static const char *const k_rs_init_general_on[] = {
    "OUTP:GEN ON",     /* arm the master output (per-channel OUTP:SEL still gates each) */
    NULL,
};

static const scpi_psu_profile_t k_rs_hmp4040 = {
    .model_name = "R&S HMP4040",
    .n_channels = 4,
    .v_max = {32.05f, 32.05f, 32.05f, 32.05f},
    .i_max = {10.0f,  10.0f,  10.0f,  10.0f},
    .channel_in_command = false,
    .select_channel_fmt = "INST OUT%d",
    .set_voltage_fmt    = "VOLT %.3f",
    .set_current_fmt    = "CURR %.3f",
    .set_output_on_fmt  = "OUTP:SEL ON",
    .set_output_off_fmt = "OUTP:SEL OFF",
    .meas_voltage_fmt   = "MEAS:VOLT?",
    .meas_current_fmt   = "MEAS:CURR?",
    .meas_power_fmt     = "MEAS:POW?",
    .init_commands      = k_rs_init_general_on,
};

static const scpi_psu_profile_t k_rs_hmp4030 = {
    .model_name = "R&S HMP4030",
    .n_channels = 3,
    .v_max = {32.05f, 32.05f, 32.05f},
    .i_max = {10.0f,  10.0f,  10.0f},
    .channel_in_command = false,
    .select_channel_fmt = "INST OUT%d",
    .set_voltage_fmt    = "VOLT %.3f",
    .set_current_fmt    = "CURR %.3f",
    .set_output_on_fmt  = "OUTP:SEL ON",
    .set_output_off_fmt = "OUTP:SEL OFF",
    .meas_voltage_fmt   = "MEAS:VOLT?",
    .meas_current_fmt   = "MEAS:CURR?",
    .meas_power_fmt     = "MEAS:POW?",
    .init_commands      = k_rs_init_general_on,
};

static const scpi_psu_profile_t k_rs_hmp2030 = {
    .model_name = "R&S HMP2030",
    .n_channels = 3,
    .v_max = {32.0f, 32.0f, 32.0f},
    .i_max = { 5.0f,  5.0f,  5.0f},
    .channel_in_command = false,
    .select_channel_fmt = "INST OUT%d",
    .set_voltage_fmt    = "VOLT %.3f",
    .set_current_fmt    = "CURR %.3f",
    .set_output_on_fmt  = "OUTP:SEL ON",
    .set_output_off_fmt = "OUTP:SEL OFF",
    .meas_voltage_fmt   = "MEAS:VOLT?",
    .meas_current_fmt   = "MEAS:CURR?",
    .meas_power_fmt     = "MEAS:POW?",
    .init_commands      = k_rs_init_general_on,
};

/* NGE100B family - same SCPI shape as HMP, just smaller. NGE103B is 3-ch
 * 32V/3A; NGE102B is the 2-ch variant; NGE101B is single. */
static const scpi_psu_profile_t k_rs_nge103b = {
    .model_name = "R&S NGE103B",
    .n_channels = 3,
    .v_max = {32.0f, 32.0f, 32.0f},
    .i_max = { 3.0f,  3.0f,  3.0f},
    .channel_in_command = false,
    .select_channel_fmt = "INST OUT%d",
    .set_voltage_fmt    = "VOLT %.3f",
    .set_current_fmt    = "CURR %.3f",
    .set_output_on_fmt  = "OUTP:SEL ON",
    .set_output_off_fmt = "OUTP:SEL OFF",
    .meas_voltage_fmt   = "MEAS:VOLT?",
    .meas_current_fmt   = "MEAS:CURR?",
    .meas_power_fmt     = "MEAS:POW?",
    .init_commands      = k_rs_init_general_on,
};

/* ----- Keithley / Tektronix 2230 series -----------------------------------
 * 2230G / 2231A triple-channel. INST:SEL CH<n> selects, then VOLT/CURR
 * setpoints and CHAN:OUTP enable apply to the selected channel. No
 * MEAS:POWE? on this family - driver computes V*A. */

static const scpi_psu_profile_t k_keithley_2230g = {
    .model_name = "Keithley 2230G",
    .n_channels = 3,
    .v_max = {30.0f, 30.0f,  6.0f},   /* 2230G-30-1: CH1+CH2 30V/1.5A, CH3 6V/5A */
    .i_max = { 1.5f,  1.5f,  5.0f},
    .channel_in_command = false,
    .select_channel_fmt = "INST:SEL CH%d",
    .set_voltage_fmt    = "VOLT %.4f",
    .set_current_fmt    = "CURR %.4f",
    .set_output_on_fmt  = "CHAN:OUTP ON",
    .set_output_off_fmt = "CHAN:OUTP OFF",
    .meas_voltage_fmt   = "MEAS:VOLT?",
    .meas_current_fmt   = "MEAS:CURR?",
    .meas_power_fmt     = NULL,
};

static const scpi_psu_profile_t k_keithley_2231a = {
    .model_name = "Keithley 2231A-30-3",
    .n_channels = 3,
    .v_max = {30.0f, 30.0f, 30.0f},
    .i_max = { 3.0f,  3.0f,  3.0f},
    .channel_in_command = false,
    .select_channel_fmt = "INST:SEL CH%d",
    .set_voltage_fmt    = "VOLT %.4f",
    .set_current_fmt    = "CURR %.4f",
    .set_output_on_fmt  = "CHAN:OUTP ON",
    .set_output_off_fmt = "CHAN:OUTP OFF",
    .meas_voltage_fmt   = "MEAS:VOLT?",
    .meas_current_fmt   = "MEAS:CURR?",
    .meas_power_fmt     = NULL,
};

/* ----- HP / Agilent 663xA - classic GPIB single-output supplies ------------
 * Identical SCPI shape; only the V/I ranges differ. Defaults match the
 * 6632A (20V/5A), 6633A (50V/2A), 6634A (100V/1A). Common on GPIB; reach
 * via --port=prologix:/dev/ttyUSB0:<gpib-addr>. */

#define HP_663X_COMMON                                            \
    .channel_in_command = false,                                  \
    .select_channel_fmt = NULL,                                   \
    .set_voltage_fmt    = "VOLT %.3f",                            \
    .set_current_fmt    = "CURR %.3f",                            \
    .set_output_on_fmt  = "OUTP ON",                              \
    .set_output_off_fmt = "OUTP OFF",                             \
    .meas_voltage_fmt   = "MEAS:VOLT?",                           \
    .meas_current_fmt   = "MEAS:CURR?",                           \
    .meas_power_fmt     = NULL

static const scpi_psu_profile_t k_hp_6632a = {
    .model_name = "HP/Agilent 6632A",
    .n_channels = 1,
    .v_max = {20.0f},
    .i_max = { 5.0f},
    HP_663X_COMMON,
};

static const scpi_psu_profile_t k_hp_6633a = {
    .model_name = "HP/Agilent 6633A",
    .n_channels = 1,
    .v_max = {50.0f},
    .i_max = { 2.0f},
    HP_663X_COMMON,
};

static const scpi_psu_profile_t k_hp_6634a = {
    .model_name = "HP/Agilent 6634A",
    .n_channels = 1,
    .v_max = {100.0f},
    .i_max = {  1.0f},
    HP_663X_COMMON,
};

/* ---- driver state ---- */

#define DEFAULT_POLL_MS 250
#define QUERY_TIMEOUT_MS 600

typedef struct {
    scpi_t *scpi;
    const scpi_psu_profile_t *prof;

    pthread_t reader;
    volatile bool running;

    pthread_mutex_t state_lock;
    psu_channel_state_t state[MAX_CHANNELS];

    /* Cached setpoint / output values - instruments often don't report these
     * back symmetrically, so we trust local last-set. */
    float set_v[MAX_CHANNELS];
    float set_a[MAX_CHANNELS];
    bool  out_on[MAX_CHANNELS];

    /* For the channel-select-first style, remember the last-selected channel
     * to skip redundant INST commands. */
    int last_selected_ch;

    volatile bool connected;
    volatile uint32_t err_count;
    volatile uint32_t rx_count;
} scpi_psu_state_t;

static scpi_psu_state_t *st_of(psu_driver_t *d) { return (scpi_psu_state_t *)d->state; }

#define now_ms() pl_now_ms()

/* Send a command optionally prefixed by a channel-select. With
 * channel_in_command, the channel is baked into the command and select is
 * skipped. */
static bool send_channel_cmd(scpi_psu_state_t *s, int ch, const char *fmt_or_value_cmd,
                             bool has_value, double value) {
    char buf[160];
    if (s->prof->channel_in_command) {
        if (has_value) snprintf(buf, sizeof(buf), fmt_or_value_cmd, ch, value);
        else           snprintf(buf, sizeof(buf), fmt_or_value_cmd, ch);
    } else {
        if (s->prof->select_channel_fmt && ch != s->last_selected_ch) {
            char sel[64];
            snprintf(sel, sizeof(sel), s->prof->select_channel_fmt, ch);
            if (!scpi_send(s->scpi, sel)) {
                s->err_count++;
                return false;
            }
            s->last_selected_ch = ch;
        }
        if (has_value) snprintf(buf, sizeof(buf), fmt_or_value_cmd, value);
        else           snprintf(buf, sizeof(buf), "%s", fmt_or_value_cmd);
    }
    bool ok = scpi_send(s->scpi, buf);
    if (!ok) s->err_count++;
    return ok;
}

static bool query_float(scpi_psu_state_t *s, int ch, const char *fmt, float *out) {
    char cmd[160];
    char resp[160];

    if (s->prof->channel_in_command) {
        snprintf(cmd, sizeof(cmd), fmt, ch);
    } else {
        if (s->prof->select_channel_fmt && ch != s->last_selected_ch) {
            char sel[64];
            snprintf(sel, sizeof(sel), s->prof->select_channel_fmt, ch);
            if (!scpi_send(s->scpi, sel)) { s->err_count++; return false; }
            s->last_selected_ch = ch;
        }
        snprintf(cmd, sizeof(cmd), "%s", fmt);
    }

    if (!scpi_query(s->scpi, cmd, resp, sizeof(resp), QUERY_TIMEOUT_MS)) {
        s->err_count++;
        return false;
    }
    s->rx_count++;
    *out = (float)atof(resp);
    return true;
}

/* ---- reader thread ---- */

static void *reader_main(void *arg) {
    psu_driver_t *d = (psu_driver_t *)arg;
    scpi_psu_state_t *s = st_of(d);

    /* Identify the instrument (informational). */
    char idn[160] = {0};
    if (scpi_query(s->scpi, "*IDN?", idn, sizeof(idn), 800)) {
        fprintf(stderr, "scpi-psu: %s says: %s", s->prof->model_name, idn);
        if (strchr(idn, '\n') == NULL) fputc('\n', stderr);
        s->connected = true;
        s->rx_count++;
    } else {
        fprintf(stderr, "scpi-psu: *IDN? timed out - continuing anyway\n");
        s->err_count++;
    }

    /* Profile-defined one-off setup (e.g. R&S HMP "OUTP:GEN ON"). Failures
     * are non-fatal - the instrument might already be in the right state. */
    if (s->prof->init_commands) {
        for (const char *const *cmd = s->prof->init_commands; *cmd; cmd++) {
            if (!scpi_send(s->scpi, *cmd)) s->err_count++;
        }
    }

    while (s->running) {
        for (int ch = 1; ch <= s->prof->n_channels && s->running; ch++) {
            float v = 0, a = 0, p = 0;
            bool ok_v = query_float(s, ch, s->prof->meas_voltage_fmt, &v);
            bool ok_a = query_float(s, ch, s->prof->meas_current_fmt, &a);
            bool ok_p = false;
            if (s->prof->meas_power_fmt)
                ok_p = query_float(s, ch, s->prof->meas_power_fmt, &p);
            if (!ok_p) p = v * a;

            pthread_mutex_lock(&s->state_lock);
            psu_channel_state_t *cs = &s->state[ch - 1];
            cs->set_v = s->set_v[ch - 1];
            cs->set_a = s->set_a[ch - 1];
            if (ok_v) cs->out_v = v;
            if (ok_a) cs->out_a = a;
            cs->out_p = p;
            cs->out_on = s->out_on[ch - 1];
            /* Heuristic CV/CC: in CC the output current is close to the
             * setpoint and the output voltage has dropped below the setting. */
            cs->cv_mode = (s->out_on[ch - 1] && cs->out_v < cs->set_v - 0.05f
                           && cs->out_a >= cs->set_a - 0.01f) ? false : true;
            cs->valid = (ok_v && ok_a);
            if (cs->valid) {
                cs->timestamp_ms = now_ms();
                s->connected = true;
            }
            pthread_mutex_unlock(&s->state_lock);
        }
        /* Coarse poll; SCPI/GPIB instruments don't like being hammered. */
        for (int i = 0; i < DEFAULT_POLL_MS / 20 && s->running; i++)
            pl_sleep_ms(20);
    }
    return NULL;
}

/* ---- vtable ---- */

static void v_close(psu_driver_t *self) {
    if (!self) return;
    scpi_psu_state_t *s = st_of(self);
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
    scpi_psu_state_t *s = st_of(self);
    if (ch < 1 || ch > s->prof->n_channels) { memset(out, 0, sizeof(*out)); return; }
    pthread_mutex_lock(&s->state_lock);
    *out = s->state[ch - 1];
    pthread_mutex_unlock(&s->state_lock);
}

static bool v_set_voltage(psu_driver_t *self, int ch, float v) {
    scpi_psu_state_t *s = st_of(self);
    if (ch < 1 || ch > s->prof->n_channels) return false;
    if (v < 0 || v > s->prof->v_max[ch - 1]) return false;
    pthread_mutex_lock(&s->state_lock); s->set_v[ch - 1] = v; pthread_mutex_unlock(&s->state_lock);
    return send_channel_cmd(s, ch, s->prof->set_voltage_fmt, true, (double)v);
}

static bool v_set_current(psu_driver_t *self, int ch, float a) {
    scpi_psu_state_t *s = st_of(self);
    if (ch < 1 || ch > s->prof->n_channels) return false;
    if (a < 0 || a > s->prof->i_max[ch - 1]) return false;
    pthread_mutex_lock(&s->state_lock); s->set_a[ch - 1] = a; pthread_mutex_unlock(&s->state_lock);
    return send_channel_cmd(s, ch, s->prof->set_current_fmt, true, (double)a);
}

static bool v_set_output(psu_driver_t *self, int ch, bool on) {
    scpi_psu_state_t *s = st_of(self);
    if (ch < 1 || ch > s->prof->n_channels) return false;
    pthread_mutex_lock(&s->state_lock); s->out_on[ch - 1] = on; pthread_mutex_unlock(&s->state_lock);
    const char *fmt = on ? s->prof->set_output_on_fmt : s->prof->set_output_off_fmt;
    return send_channel_cmd(s, ch, fmt, false, 0.0);
}

/* Global tracking toggle (no channel argument). Only wired into the vtable
 * when the profile defines both commands. */
static bool v_set_tracking(psu_driver_t *self, bool on) {
    scpi_psu_state_t *s = st_of(self);
    const char *cmd = on ? s->prof->set_tracking_on : s->prof->set_tracking_off;
    if (!cmd) return false;
    bool ok = scpi_send(s->scpi, cmd);
    if (!ok) s->err_count++;
    return ok;
}

static void v_get_stats(psu_driver_t *self, uint32_t *rx, uint32_t *err) {
    scpi_psu_state_t *s = st_of(self);
    if (rx)  *rx  = s->rx_count;
    if (err) *err = s->err_count;
}

/* ---- factory shared open() ---- */

static psu_driver_t *scpi_psu_open(const scpi_psu_profile_t *prof,
                                   const char *port_spec,
                                   int default_baud) {
    if (!prof || !port_spec) return NULL;

    scpi_t *scpi = scpi_open(port_spec, default_baud);
    if (!scpi) {
        fprintf(stderr, "scpi-psu: failed to open transport for '%s'\n", port_spec);
        return NULL;
    }

    scpi_psu_state_t *s = calloc(1, sizeof(*s));
    psu_driver_t     *d = calloc(1, sizeof(*d));
    if (!s || !d) { free(s); free(d); scpi_close(scpi); return NULL; }

    s->scpi    = scpi;
    s->prof    = prof;
    s->running = true;
    s->last_selected_ch = -1;
    pthread_mutex_init(&s->state_lock, NULL);

    /* Sensible defaults for the local cache. */
    for (int i = 0; i < prof->n_channels; i++) {
        s->set_v[i] = 0.0f;
        s->set_a[i] = prof->i_max[i] * 0.1f;   /* 10% as a safety default */
        s->out_on[i] = false;
        s->state[i].set_v = s->set_v[i];
        s->state[i].set_a = s->set_a[i];
    }

    d->state = s;
    /* Use the largest per-channel limit as the GUI bound. */
    float v_lim = 0, i_lim = 0;
    for (int i = 0; i < prof->n_channels; i++) {
        if (prof->v_max[i] > v_lim) v_lim = prof->v_max[i];
        if (prof->i_max[i] > i_lim) i_lim = prof->i_max[i];
    }
    bool has_tracking = (prof->set_tracking_on != NULL && prof->set_tracking_off != NULL);
    d->caps = (psu_caps_t){
        .model_name             = prof->model_name,
        .n_channels             = prof->n_channels,
        .v_max                  = v_lim,
        .i_max                  = i_lim,
        .supports_tracking      = has_tracking,
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
    d->set_tracking = has_tracking ? v_set_tracking : NULL;
    d->get_stats    = v_get_stats;

    if (pthread_create(&s->reader, NULL, reader_main, d) != 0) {
        v_close(d);
        return NULL;
    }
    return d;
}

/* ---- per-model factory functions ---- */

static psu_driver_t *open_siglent_spd3303(const char *p, int b) { return scpi_psu_open(&k_siglent_spd3303, p, b); }
static psu_driver_t *open_keysight_e3631a(const char *p, int b) { return scpi_psu_open(&k_keysight_e3631a, p, b); }
static psu_driver_t *open_keysight_e3633a(const char *p, int b) { return scpi_psu_open(&k_keysight_e3633a, p, b); }
static psu_driver_t *open_keysight_e3634a(const char *p, int b) { return scpi_psu_open(&k_keysight_e3634a, p, b); }
static psu_driver_t *open_keysight_e3645a(const char *p, int b) { return scpi_psu_open(&k_keysight_e3645a, p, b); }

static psu_driver_t *open_rigol_dp832(const char *p, int b)     { return scpi_psu_open(&k_rigol_dp832,     p, b); }
static psu_driver_t *open_rigol_dp832a(const char *p, int b)    { return scpi_psu_open(&k_rigol_dp832a,    p, b); }
static psu_driver_t *open_rigol_dp811(const char *p, int b)     { return scpi_psu_open(&k_rigol_dp811,     p, b); }
static psu_driver_t *open_rigol_dp711(const char *p, int b)     { return scpi_psu_open(&k_rigol_dp711,     p, b); }

static psu_driver_t *open_rs_hmp4040(const char *p, int b)      { return scpi_psu_open(&k_rs_hmp4040,      p, b); }
static psu_driver_t *open_rs_hmp4030(const char *p, int b)      { return scpi_psu_open(&k_rs_hmp4030,      p, b); }
static psu_driver_t *open_rs_hmp2030(const char *p, int b)      { return scpi_psu_open(&k_rs_hmp2030,      p, b); }
static psu_driver_t *open_rs_nge103b(const char *p, int b)      { return scpi_psu_open(&k_rs_nge103b,      p, b); }

static psu_driver_t *open_keithley_2230g(const char *p, int b)  { return scpi_psu_open(&k_keithley_2230g,  p, b); }
static psu_driver_t *open_keithley_2231a(const char *p, int b)  { return scpi_psu_open(&k_keithley_2231a,  p, b); }

static psu_driver_t *open_hp_6632a(const char *p, int b)        { return scpi_psu_open(&k_hp_6632a,        p, b); }
static psu_driver_t *open_hp_6633a(const char *p, int b)        { return scpi_psu_open(&k_hp_6633a,        p, b); }
static psu_driver_t *open_hp_6634a(const char *p, int b)        { return scpi_psu_open(&k_hp_6634a,        p, b); }

const psu_driver_factory_t siglent_spd_factory = {
    .id              = "siglent-spd",
    .display_name    = "Siglent SPD3303 (SCPI)",
    .description     = "Siglent SPD-series via SCPI; serial: or prologix: transport",
    .default_baud    = 115200,
    .n_channels_hint = 2,
    .open            = open_siglent_spd3303,
};

const psu_driver_factory_t keysight_e3631a_factory = {
    .id              = "keysight-e3631a",
    .display_name    = "Keysight/Agilent/HP E3631A (SCPI)",
    .description     = "Triple-output: +6V/5A, +25V/1A, -25V/1A. Via serial or Prologix GPIB.",
    .default_baud    = 9600,
    .n_channels_hint = 3,
    .open            = open_keysight_e3631a,
};

const psu_driver_factory_t keysight_e3633a_factory = {
    .id              = "keysight-e3633a",
    .display_name    = "Keysight/Agilent/HP E3633A (SCPI)",
    .description     = "Single output, 8V/20A or 20V/10A. Via serial or Prologix GPIB.",
    .default_baud    = 9600,
    .n_channels_hint = 1,
    .open            = open_keysight_e3633a,
};

const psu_driver_factory_t keysight_e3634a_factory = {
    .id              = "keysight-e3634a",
    .display_name    = "Keysight/Agilent/HP E3634A (SCPI)",
    .description     = "Single output, 8V/7A or 25V/4A. Via serial or Prologix GPIB.",
    .default_baud    = 9600,
    .n_channels_hint = 1,
    .open            = open_keysight_e3634a,
};

const psu_driver_factory_t keysight_e3645a_factory = {
    .id              = "keysight-e3645a",
    .display_name    = "Keysight/Agilent/HP E3645A (SCPI)",
    .description     = "Single output, 8V/5A or 20V/2.2A. Via serial or Prologix GPIB.",
    .default_baud    = 9600,
    .n_channels_hint = 1,
    .open            = open_keysight_e3645a,
};

/* ----- Rigol DP-series factories ----------------------------------------- */

const psu_driver_factory_t rigol_dp832_factory = {
    .id              = "rigol-dp832",
    .display_name    = "Rigol DP832 (SCPI)",
    .description     = "Triple-output: CH1+CH2 30V/3A, CH3 5V/3A. Serial or Prologix GPIB.",
    .default_baud    = 9600,
    .n_channels_hint = 3,
    .open            = open_rigol_dp832,
};

const psu_driver_factory_t rigol_dp832a_factory = {
    .id              = "rigol-dp832a",
    .display_name    = "Rigol DP832A (SCPI, higher-resolution)",
    .description     = "Same outputs as DP832 with finer setpoint resolution. Serial or Prologix GPIB.",
    .default_baud    = 9600,
    .n_channels_hint = 3,
    .open            = open_rigol_dp832a,
};

const psu_driver_factory_t rigol_dp811_factory = {
    .id              = "rigol-dp811",
    .display_name    = "Rigol DP811 (SCPI)",
    .description     = "Single output: 20V/10A (or 40V/5A in low-current range). Serial or Prologix GPIB.",
    .default_baud    = 9600,
    .n_channels_hint = 1,
    .open            = open_rigol_dp811,
};

const psu_driver_factory_t rigol_dp711_factory = {
    .id              = "rigol-dp711",
    .display_name    = "Rigol DP711 (SCPI)",
    .description     = "Single output: 30V/5A linear. Serial or Prologix GPIB.",
    .default_baud    = 9600,
    .n_channels_hint = 1,
    .open            = open_rigol_dp711,
};

/* ----- Rohde & Schwarz HMP / NGE factories ------------------------------- */

const psu_driver_factory_t rs_hmp4040_factory = {
    .id              = "rs-hmp4040",
    .display_name    = "R&S HMP4040 (SCPI)",
    .description     = "Quad output, each 32V/10A. Master output enabled at open. Serial or GPIB.",
    .default_baud    = 9600,
    .n_channels_hint = 4,
    .open            = open_rs_hmp4040,
};

const psu_driver_factory_t rs_hmp4030_factory = {
    .id              = "rs-hmp4030",
    .display_name    = "R&S HMP4030 (SCPI)",
    .description     = "Triple output, each 32V/10A. Master output enabled at open. Serial or GPIB.",
    .default_baud    = 9600,
    .n_channels_hint = 3,
    .open            = open_rs_hmp4030,
};

const psu_driver_factory_t rs_hmp2030_factory = {
    .id              = "rs-hmp2030",
    .display_name    = "R&S HMP2030 (SCPI)",
    .description     = "Triple output, each 32V/5A. Master output enabled at open. Serial or GPIB.",
    .default_baud    = 9600,
    .n_channels_hint = 3,
    .open            = open_rs_hmp2030,
};

const psu_driver_factory_t rs_nge103b_factory = {
    .id              = "rs-nge103b",
    .display_name    = "R&S NGE103B (SCPI)",
    .description     = "Triple output, each 32V/3A. Master output enabled at open. Serial or GPIB.",
    .default_baud    = 9600,
    .n_channels_hint = 3,
    .open            = open_rs_nge103b,
};

/* ----- Keithley / Tektronix factories ----------------------------------- */

const psu_driver_factory_t keithley_2230g_factory = {
    .id              = "keithley-2230g",
    .display_name    = "Keithley 2230G (SCPI)",
    .description     = "Triple output: 30V/1.5A x2 + 6V/5A. Serial or Prologix GPIB.",
    .default_baud    = 9600,
    .n_channels_hint = 3,
    .open            = open_keithley_2230g,
};

const psu_driver_factory_t keithley_2231a_factory = {
    .id              = "keithley-2231a",
    .display_name    = "Keithley 2231A-30-3 (SCPI)",
    .description     = "Triple output, each 30V/3A. Serial or Prologix GPIB.",
    .default_baud    = 9600,
    .n_channels_hint = 3,
    .open            = open_keithley_2231a,
};

/* ----- HP / Agilent 663xA factories (classic single-output) -------------- */

const psu_driver_factory_t hp_6632a_factory = {
    .id              = "hp-6632a",
    .display_name    = "HP/Agilent 6632A (SCPI)",
    .description     = "Single output, 20V/5A. Typically on GPIB - use prologix:<dev>:<addr>.",
    .default_baud    = 9600,
    .n_channels_hint = 1,
    .open            = open_hp_6632a,
};

const psu_driver_factory_t hp_6633a_factory = {
    .id              = "hp-6633a",
    .display_name    = "HP/Agilent 6633A (SCPI)",
    .description     = "Single output, 50V/2A. Typically on GPIB.",
    .default_baud    = 9600,
    .n_channels_hint = 1,
    .open            = open_hp_6633a,
};

const psu_driver_factory_t hp_6634a_factory = {
    .id              = "hp-6634a",
    .display_name    = "HP/Agilent 6634A (SCPI)",
    .description     = "Single output, 100V/1A. Typically on GPIB.",
    .default_baud    = 9600,
    .n_channels_hint = 1,
    .open            = open_hp_6634a,
};
