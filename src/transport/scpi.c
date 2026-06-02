/**
 * SCPI client — top-level dispatcher and locked-wrapper API.
 *
 * Concrete transports live in scpi_serial.c and scpi_prologix.c. Each one
 * exposes an scpi_*_open() factory that fills in the vtable on the returned
 * scpi_t. The locking lives here so transports don't each implement it.
 */

#include "scpi.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void scpi_close(scpi_t *s) {
    if (!s) return;
    if (s->close_impl) s->close_impl(s);
    pthread_mutex_destroy(&s->lock);
    free(s);
}

bool scpi_send(scpi_t *s, const char *cmd) {
    if (!s || !s->send_impl || !cmd) return false;
    pthread_mutex_lock(&s->lock);
    bool ok = s->send_impl(s, cmd);
    pthread_mutex_unlock(&s->lock);
    return ok;
}

bool scpi_query(scpi_t *s, const char *cmd, char *out, size_t outlen,
                int timeout_ms) {
    if (!s || !s->send_impl || !s->recv_impl || !cmd || !out || outlen == 0)
        return false;
    pthread_mutex_lock(&s->lock);
    bool ok = s->send_impl(s, cmd) &&
              s->recv_impl(s, out, outlen, timeout_ms);
    pthread_mutex_unlock(&s->lock);
    return ok;
}

/* ---- spec parsing ---- */

/* Pull the next colon-separated field from *p into out (max outlen). Advances
 * *p past the field (and the separator). Returns true if a field was read. */
static bool next_field(const char **p, char *out, size_t outlen) {
    if (!*p || !**p) return false;
    const char *start = *p;
    const char *end = strchr(start, ':');
    size_t n = end ? (size_t)(end - start) : strlen(start);
    if (n >= outlen) n = outlen - 1;
    memcpy(out, start, n);
    out[n] = '\0';
    *p = end ? (end + 1) : (start + strlen(start));
    return true;
}

scpi_t *scpi_open(const char *port_spec, int default_baud) {
    if (!port_spec || !*port_spec) return NULL;

    /* Bare path → assume serial. */
    if (port_spec[0] == '/') {
        return scpi_serial_open(port_spec, default_baud > 0 ? default_baud : 115200);
    }

    char scheme[32];
    const char *p = port_spec;
    if (!next_field(&p, scheme, sizeof(scheme))) return NULL;

    if (strcmp(scheme, "serial") == 0) {
        char device[256] = {0};
        char baud_s[16] = {0};
        if (!next_field(&p, device, sizeof(device)) || !*device) return NULL;
        next_field(&p, baud_s, sizeof(baud_s));
        int baud = baud_s[0] ? atoi(baud_s) : (default_baud > 0 ? default_baud : 115200);
        return scpi_serial_open(device, baud);
    }

    if (strcmp(scheme, "prologix") == 0) {
        char device[256] = {0};
        char addr_s[16]  = {0};
        char baud_s[16]  = {0};
        if (!next_field(&p, device, sizeof(device)) || !*device) return NULL;
        if (!next_field(&p, addr_s,  sizeof(addr_s))  || !*addr_s) {
            fprintf(stderr, "scpi: prologix:<dev>:<gpib-addr> — missing GPIB address\n");
            return NULL;
        }
        next_field(&p, baud_s, sizeof(baud_s));
        int addr = atoi(addr_s);
        int baud = baud_s[0] ? atoi(baud_s) : 115200;  /* Prologix default */
        if (addr < 0 || addr > 30) {
            fprintf(stderr, "scpi: prologix GPIB address out of range: %d\n", addr);
            return NULL;
        }
        return scpi_prologix_open(device, baud, addr);
    }

    if (strcmp(scheme, "usbtmc") == 0) {
        /* Pass the whole remainder verbatim — the USB-TMC backend handles
         * both "/dev/usbtmc0" and "<vid>:<pid>[:<serial>]" forms and the
         * vid:pid form has its own colon-separators to preserve. */
        if (!p || !*p) {
            fprintf(stderr, "scpi: usbtmc:<dev>|<vid>:<pid>[:<serial>] — missing target\n");
            return NULL;
        }
        return scpi_usbtmc_open(p);
    }

    if (strcmp(scheme, "vxi11") == 0) {
        char host[128] = {0};
        char dev[64]   = {0};
        if (!next_field(&p, host, sizeof(host)) || !*host) {
            fprintf(stderr, "scpi: vxi11:<host>[:<device>] — missing host\n");
            return NULL;
        }
        next_field(&p, dev, sizeof(dev));    /* optional device name */
        return scpi_vxi11_open(host, dev[0] ? dev : NULL);
    }

    if (strcmp(scheme, "hislip") == 0) {
        char host[128]  = {0};
        char port_s[16] = {0};
        char sub[64]    = {0};
        if (!next_field(&p, host, sizeof(host)) || !*host) {
            fprintf(stderr, "scpi: hislip:<host>[:<port>][:<sub-address>] — missing host\n");
            return NULL;
        }
        next_field(&p, port_s, sizeof(port_s));   /* optional port */
        next_field(&p, sub,    sizeof(sub));      /* optional sub-address */
        int port = port_s[0] ? atoi(port_s) : 0;
        return scpi_hislip_open(host, port, sub[0] ? sub : NULL);
    }

    fprintf(stderr, "scpi: unknown transport scheme '%s' "
                    "(expected serial: / prologix: / usbtmc: / vxi11: / hislip:)\n",
            scheme);
    return NULL;
}
